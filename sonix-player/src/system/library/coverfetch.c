#include "coverfetch.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/core/utils.h"
#include "src/system/library/albumart.h"
#include "src/system/library/library.h"
#include "src/system/net/http.h"

#ifndef COVERFETCH_MB_API
#define COVERFETCH_MB_API "https://musicbrainz.org/ws/2/release"
#endif
#define COVERFETCH_CAA "https://coverartarchive.org/release"
// A MusicBrainz search can keep the connection quiet for a while, and the TLS
// handshake on this CPU takes a few more seconds.
#define COVERFETCH_TIMEOUT_SECS 25
// 404 answers the search; 429 and 5xx (and no status at all) are transient and
// get a few more tries with a growing pause between them.
#define COVERFETCH_ATTEMPTS 3
#ifdef COVERFETCH_TEST
#define COVERFETCH_RETRY_PAUSE_SECS 0
#else
#define COVERFETCH_RETRY_PAUSE_SECS 2
#endif
// The JSON is small; a cap keeps a broken server from eating the heap.
#define COVERFETCH_MAX_BYTES (256 * 1024)
// An image is capped at 4 MB. Read one byte over so an oversized body is
// rejected instead of being silently truncated to the cap by http_get_ex().
#define COVERFETCH_MAX_IMAGE_BYTES (4 * 1024 * 1024)
#define COVERFETCH_IMAGE_NAME "cover.jpg"
// PID plus a per-save serial keeps concurrent writers in different processes
// and threads from sharing one temp path in the same album folder.
#define COVERFETCH_PART_FORMAT ".coverfetch.%ld.%lu.part"
#define COVERFETCH_MAX_RESULTS 5
#define COVERFETCH_MAX_KEYS 256

// One request at a time across every caller, held for the whole attempt loop.
static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
// Guarded by http_lock, already taken for the request. Only the name is
// serialised; the file write itself still happens after the unlock.
static unsigned long save_seq = 0;

// "not found" keys. A fixed table is enough for a library of a few hundred
// albums; when it fills, the oldest miss is dropped.
static char negative_keys[COVERFETCH_MAX_KEYS][640];
static int negative_count = 0;

// Set by coverfetch_batch_cancel(), read by the one batch worker. Volatile is
// enough for a flag that is only ever set, never used to hand over data.
static volatile bool batch_cancel = false;

#ifdef COVERFETCH_TEST
static int records_outstanding = 0;
static char test_save_dir[1024];
static char test_save_temp[1200];

int coverfetch_test_outstanding_records(void) { return records_outstanding; }

void coverfetch_test_set_dir(const char *dir) {
	snprintf(test_save_dir, sizeof(test_save_dir), "%s", dir ? dir : "");
}

const char *coverfetch_test_last_temp(void) { return test_save_temp; }
#endif

bool coverfetch_negative_has(const char *key) {
	if (!key || !key[0]) {
		return false;
	}
	for (int i = 0; i < negative_count; i++) {
		if (strcmp(negative_keys[i], key) == 0) {
			return true;
		}
	}
	return false;
}

void coverfetch_negative_add(const char *key) {
	if (!key || !key[0] || coverfetch_negative_has(key)) {
		return;
	}
	if (negative_count >= COVERFETCH_MAX_KEYS) {
		memmove(negative_keys, negative_keys + 1, sizeof(negative_keys) - sizeof(negative_keys[0]));
		negative_count--;
	}
	snprintf(negative_keys[negative_count], sizeof(negative_keys[0]), "%s", key);
	negative_count++;
}

void coverfetch_negative_clear(void) { negative_count = 0; }

void coverfetch_key(char *out, size_t size, const char *album, const char *artist) {
	if (!size) {
		return;
	}
	const char *parts[2] = {album, artist};
	size_t o = 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass) {
			if (o + 1 >= size) {
				break;
			}
			out[o++] = '|';
		}
		const char *s = parts[pass] ? parts[pass] : "";
		for (size_t i = 0; s[i] && o + 1 < size; i++) {
			out[o++] = (char)tolower((unsigned char)s[i]);
		}
	}
	out[o] = '\0';
}

void coverfetch_mb_query(char *out, size_t size, const char *album, const char *artist) {
	char a[768], r[768];
	http_url_encode(album ? album : "", a, sizeof(a));
	http_url_encode(artist ? artist : "", r, sizeof(r));
	// The two separator spaces are encoded too: http_get_ex() puts the URL
	// straight into the request line, where a literal space would split it.
	snprintf(out, size, "query=release:%s%%20AND%%20artist:%s", a, r);
}

static coverfetch_record_t *records_alloc(int n) {
	coverfetch_record_t *p = calloc((size_t)n, sizeof(*p));
#ifdef COVERFETCH_TEST
	if (p) {
		records_outstanding++;
	}
#endif
	return p;
}

// GET with the retry rules above. Called with the lock held by api_get().
static bool request(const char *url, char **body, int *status) {
	for (int attempt = 1; attempt <= COVERFETCH_ATTEMPTS; attempt++) {
		*status = 0;
		if (http_get_ex(url, NULL, false, body, NULL, COVERFETCH_MAX_BYTES, COVERFETCH_TIMEOUT_SECS, status)) {
			return true;
		}
		bool transient = *status <= 0 || *status == 429 || *status >= 500;
#ifndef COVERFETCH_TEST
		const char *why = http_last_error();
		fprintf(stderr, "coverfetch: attempt %d/%d failed (HTTP %d%s%s)\n", attempt, COVERFETCH_ATTEMPTS, *status,
				why ? ": " : "", why ? why : "");
#endif
		if (!transient || attempt == COVERFETCH_ATTEMPTS) {
			break;
		}
		sleep((unsigned)(COVERFETCH_RETRY_PAUSE_SECS * attempt));
	}
	return false;
}

static bool api_get(const char *url, char **body, int *status) {
	pthread_mutex_lock(&http_lock);
	bool ok = request(url, body, status);
	pthread_mutex_unlock(&http_lock);
	return ok;
}

coverfetch_result_t coverfetch_search(const char *album, const char *artist, const char *title,
									  coverfetch_record_t **records, int *count) {
	(void)title; // kept for disambiguation when the result list is ranked
	*records = NULL;
	*count = 0;

	if ((!album || !album[0]) && (!artist || !artist[0])) {
		return COVERFETCH_NO_TAGS;
	}

	char key[640];
	coverfetch_key(key, sizeof(key), album, artist);
	if (coverfetch_negative_has(key)) {
		return COVERFETCH_NOT_FOUND;
	}

	char query[2400];
	coverfetch_mb_query(query, sizeof(query), album, artist);
	char url[3200];
	int n = snprintf(url, sizeof(url), "%s/?%s&fmt=json&limit=%d", COVERFETCH_MB_API, query, COVERFETCH_MAX_RESULTS);
	if (n < 0 || (size_t)n >= sizeof(url)) {
		return COVERFETCH_NETWORK;
	}

	char *body = NULL;
	int status = 0;
	if (!api_get(url, &body, &status)) {
		free(body);
		if (status == 404) {
			coverfetch_negative_add(key);
			return COVERFETCH_NOT_FOUND;
		}
		return COVERFETCH_NETWORK;
	}

	coverfetch_result_t result = COVERFETCH_NETWORK;
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int root = json_root(&doc);
		if (root >= 0 && doc.toks[root].type == JSON_OBJECT) {
			int releases = json_get(&doc, root, "releases");
			if (releases >= 0 && doc.toks[releases].type == JSON_ARRAY) {
				// A parsed releases array is an answer: no usable records in it
				// means "not found", not a network failure.
				result = COVERFETCH_NOT_FOUND;
				int total = json_len(&doc, releases);
				if (total > COVERFETCH_MAX_RESULTS) {
					total = COVERFETCH_MAX_RESULTS;
				}
				coverfetch_record_t *list = total > 0 ? records_alloc(total) : NULL;
				int found = 0;
				char id[128];
				for (int i = 0; list && i < total; i++) {
					int release = json_at(&doc, releases, i);
					if (release < 0 || doc.toks[release].type != JSON_OBJECT) {
						continue;
					}
					id[0] = '\0';
					json_obj_str(&doc, release, "id", id, sizeof(id));
					if (!id[0]) {
						continue;
					}
					coverfetch_record_t *r = &list[found];
					snprintf(r->url, sizeof(r->url), "%s/%s/front-500", COVERFETCH_CAA, id);
					if (!json_obj_str(&doc, release, "title", r->source, sizeof(r->source)) || !r->source[0]) {
						snprintf(r->source, sizeof(r->source), "%s", "MusicBrainz");
					}
					r->size = 500;
					found++;
				}
				if (found > 0) {
					*records = list;
					*count = found;
					result = COVERFETCH_OK;
				} else {
					coverfetch_free(list, total);
				}
			}
		}
		json_free(&doc);
	}
	free(body);

	if (result == COVERFETCH_NOT_FOUND) {
		coverfetch_negative_add(key);
	}
#ifndef COVERFETCH_TEST
	fprintf(stderr, "coverfetch: '%s' / '%s': %d\n", album ? album : "", artist ? artist : "", (int)result);
#endif
	return result;
}

void coverfetch_free(coverfetch_record_t *records, int count) {
	(void)count;
	if (!records) {
		return;
	}
#ifdef COVERFETCH_TEST
	records_outstanding--;
#endif
	free(records);
}

// Same rules as albumart.c:43-61, whose copy is file-local. Only these three
// families reach the image decoder, and a real image needs at least 12 bytes.
static bool is_supported_image(const uint8_t *data, size_t size) {
	if (!data || size < 12) {
		return false;
	}
	if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
		return true;
	}
	static const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	if (memcmp(data, png_magic, sizeof(png_magic)) == 0) {
		return true;
	}
	if (memcmp(data, "GIF8", 4) == 0 && (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
		return true;
	}
	return false;
}

bool coverfetch_save(const char *track_path, const char *dir_path, const coverfetch_record_t *record, bool replace) {
	if (!record || !record->url[0]) {
		return false;
	}

	char derived[1024];
	const char *dir = dir_path;
#ifdef COVERFETCH_TEST
	if (test_save_dir[0]) {
		dir = test_save_dir;
	}
#endif
	if (!dir || !dir[0]) {
		if (!track_path || !track_path[0]) {
			return false;
		}
		const char *slash = strrchr(track_path, '/');
		if (!slash) {
			dir = ".";
		} else {
			size_t n = (size_t)(slash - track_path);
			if (n >= sizeof(derived)) {
				return false;
			}
			memcpy(derived, track_path, n);
			derived[n] = '\0';
			dir = derived;
		}
	}

	char target[1200];
	int n = snprintf(target, sizeof(target), "%s/%s", dir, COVERFETCH_IMAGE_NAME);
	if (n < 0 || (size_t)n >= sizeof(target)) {
		return false;
	}

	// Gap protection first: no request at all when there is nothing to fill.
	struct stat st;
	if (!replace && stat(target, &st) == 0) {
		return false;
	}

	// The same one-request-at-a-time rule as the search, held only for the
	// request itself -- writing the file must not block another caller.
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	unsigned long seq;
	pthread_mutex_lock(&http_lock);
	seq = ++save_seq;
	bool ok = http_get_ex(record->url, NULL, false, &body, &len, COVERFETCH_MAX_IMAGE_BYTES + 1,
						  COVERFETCH_TIMEOUT_SECS, &status);
	pthread_mutex_unlock(&http_lock);
	if (!ok || !body || len < 12 || len > COVERFETCH_MAX_IMAGE_BYTES ||
		!is_supported_image((const uint8_t *)body, len)) {
		free(body);
		return false;
	}

	char temp[1200];
	n = snprintf(temp, sizeof(temp), "%s" COVERFETCH_PART_FORMAT, target, (long)getpid(), seq);
	if (n < 0 || (size_t)n >= sizeof(temp)) {
		free(body);
		return false;
	}
#ifdef COVERFETCH_TEST
	snprintf(test_save_temp, sizeof(test_save_temp), "%s", temp);
#endif

	FILE *f = fopen(temp, "wb");
	if (!f) {
		free(body);
		return false;
	}
	bool good = fwrite(body, 1, len, f) == len;
	if (good) {
		good = fflush(f) == 0;
	}
	if (good) {
		good = fsync(fileno(f)) == 0;
	}
	if (fclose(f) != 0) {
		good = false;
	}
	free(body);

	if (!good || rename(temp, target) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Batch download
//
// One worker walks the jobs in order, sharing the single request lock with the
// other callers. It copies the jobs so a page that started the batch can go
// away without leaving the worker holding freed memory. Progress goes through
// the caller's callback, which posts it to the interface thread.
// ---------------------------------------------------------------------------

void coverfetch_batch_cancel(void) { batch_cancel = true; }

#ifndef COVERFETCH_TEST
// An album row already carries one representative track in `path` (see
// library.c:2482). Use it directly: library_for_each() holds the index lock
// while calling back, so a nested walk here would deadlock.
static bool missing_album_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	int *missing = user;
	if (!path || !path[0]) {
		return true;
	}
	albumart_t art;
	memset(&art, 0, sizeof(art));
	bool has_art = albumart_load_for_file(path, &art);
	albumart_free(&art);
	if (!has_art) {
		(*missing)++;
	}
	return true;
}

int coverfetch_batch_missing(void) {
	int missing = 0;
	library_for_each(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, missing_album_cb, &missing);
	return missing;
}
#else
// The host test drives jobs directly and does not pull in the library.
int coverfetch_batch_missing(void) { return 0; }
#endif

typedef struct {
	coverfetch_job_t *jobs;
	int count;
	coverfetch_progress_cb cb;
	void *user;
} batch_ctx_t;

static void *batch_worker(void *arg) {
	batch_ctx_t *ctx = arg;
#ifndef COVERFETCH_TEST
	thread_be_background("cover fetch");
#endif

	coverfetch_progress_t p;
	memset(&p, 0, sizeof(p));
	p.total = ctx->count;

	for (int i = 0; i < ctx->count; i++) {
		if (batch_cancel) {
			break;
		}
		const coverfetch_job_t *job = &ctx->jobs[i];
		char key[640];
		coverfetch_key(key, sizeof(key), job->album, job->artist);
		if (coverfetch_negative_has(key)) {
			// An earlier run already asked about this album and got nothing.
			p.not_found++;
		} else {
			coverfetch_record_t *records = NULL;
			int count = 0;
			coverfetch_result_t r = coverfetch_search(job->album, job->artist, "", &records, &count);
			if (r == COVERFETCH_OK && count > 0 && coverfetch_save(job->path, job->dir, &records[0], false)) {
				p.found++;
			} else {
				p.not_found++;
			}
			coverfetch_free(records, count);
		}
		p.done++;

		// Check again after the blocking work: a cancel during the request
		// stops here rather than posting an answer nobody wants.
		if (batch_cancel) {
			break;
		}
		if (ctx->cb) {
			p.cancelled = batch_cancel;
			ctx->cb(&p, ctx->user);
		}
	}

	free(ctx->jobs);
	free(ctx);
	return NULL;
}

void coverfetch_batch_run(const coverfetch_job_t *jobs, int count, coverfetch_progress_cb cb, void *user) {
	if (!jobs || count <= 0) {
		return;
	}
	batch_cancel = false;

	batch_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return;
	}
	ctx->jobs = calloc((size_t)count, sizeof(*ctx->jobs));
	if (!ctx->jobs) {
		free(ctx);
		return;
	}
	memcpy(ctx->jobs, jobs, (size_t)count * sizeof(*jobs));
	ctx->count = count;
	ctx->cb = cb;
	ctx->user = user;

#ifdef COVERFETCH_TEST
	// Deterministic for the host test: run to completion on this thread so the
	// case can inspect the final progress and the stub's call count directly.
	batch_worker(ctx);
#else
	pthread_t thread;
	if (pthread_create(&thread, NULL, batch_worker, ctx) != 0) {
		free(ctx->jobs);
		free(ctx);
		return;
	}
	pthread_detach(thread);
#endif
}
