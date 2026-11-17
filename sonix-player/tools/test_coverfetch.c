// The cover fetch client, on the host, against stubbed HTTP and the real JSON
// parser. Built and run by `make test-coverfetch`. Not part of host or target.

#include "src/system/library/coverfetch.h"
#include "src/system/library/coverfetch_test.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The HTTP layer, replaced here so no request leaves the process. The encoder
// mirrors the production one in http.c: every non-alphanumeric byte (spaces
// included) becomes %XX, so the query assertions test the real assembly.
static char stub_body[65536];
static char stub_url[2048];
static int stub_status = 200;
static int stub_calls = 0;
static char stub_last_error[256];
// Downloads are binary and need an explicit length: a cover can contain NULs
// and can be far larger than the NUL-terminated JSON search response above.
static unsigned char *stub_download = NULL;
static size_t stub_download_len = 0;
static bool stub_download_mode = false;

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out,
				 size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;
	stub_calls++;
	snprintf(stub_url, sizeof(stub_url), "%s", url ? url : "");
	*status_out = stub_status;
	if (stub_status < 200 || stub_status >= 300) {
		*out = NULL;
		return false;
	}
	if (stub_download_mode) {
		size_t len = stub_download_len;
		*out = malloc(len ? len : 1);
		if (!*out) {
			return false;
		}
		if (len) {
			memcpy(*out, stub_download, len);
		}
		if (out_len) {
			*out_len = len;
		}
		return true;
	}
	size_t len = strlen(stub_body);
	*out = malloc(len + 1);
	if (!*out) {
		return false;
	}
	memcpy(*out, stub_body, len + 1);
	if (out_len) {
		*out_len = len;
	}
	return true;
}

const char *http_last_error(void) { return stub_last_error; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;
	for (size_t i = 0; in && in[i] && o + 4 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];
		bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
					c == '_' || c == '.' || c == '~';
		if (safe) {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = HEX[c >> 4];
			out[o++] = HEX[c & 0x0f];
		}
	}
	if (out_size) {
		out[o < out_size ? o : out_size - 1] = '\0';
	}
	return out;
}

// The hooks that set those stubs up. Implemented here beside the state they
// drive; coverfetch_test.h declares them.
void coverfetch_test_reset(void) {
	stub_body[0] = '\0';
	stub_url[0] = '\0';
	stub_status = 200;
	stub_calls = 0;
	stub_last_error[0] = '\0';
	free(stub_download);
	stub_download = NULL;
	stub_download_len = 0;
	stub_download_mode = false;
	coverfetch_negative_clear();
}

void coverfetch_test_set_body(const char *body) { snprintf(stub_body, sizeof(stub_body), "%s", body ? body : ""); }

void coverfetch_test_set_download(const void *data, size_t len) {
	free(stub_download);
	stub_download = NULL;
	stub_download_len = len;
	stub_download_mode = true;
	if (len) {
		stub_download = malloc(len);
		if (!stub_download) {
			printf("FAIL coverfetch_test_set_download: out of memory\n");
			exit(1);
		}
		memcpy(stub_download, data, len);
	}
}

void coverfetch_test_set_status(int status) { stub_status = status; }

int coverfetch_test_calls(void) { return stub_calls; }

const char *coverfetch_test_url(void) { return stub_url; }

#define CHECK(cond, name) \
	do { \
		if (cond) { \
			printf("ok   %s\n", name); \
		} else { \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
			exit(1); \
		} \
	} while (0)

// Two MusicBrainz releases, enough to check record building and freeing.
static const char *MB_BODY =
	"{\"releases\":["
	"{\"id\":\"rel-aaa\",\"title\":\"Abbey Road\"},"
	"{\"id\":\"rel-bbb\",\"title\":\"Let It Be\"}"
	"]}";

// 1. The built query encodes every space, and is what search sends.
static void case_query_encodes_album_and_artist(void) {
	char q[512];
	coverfetch_mb_query(q, sizeof(q), "Abbey Road", "The Beatles");
	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	coverfetch_free(records, count);
	const char *pinned = "query=release:Abbey%20Road%20AND%20artist:The%20Beatles";
	CHECK(strcmp(q, pinned) == 0 && strstr(coverfetch_test_url(), pinned) != NULL,
		  "query_encodes_album_and_artist");
}

// 2. A 404 is an answer: one request, not found, nothing handed back.
static void case_four_xx_not_retried(void) {
	coverfetch_test_reset();
	coverfetch_test_set_status(404);
	coverfetch_record_t *records = (coverfetch_record_t *)1;
	int count = -1;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NOT_FOUND && coverfetch_test_calls() == 1 && records == NULL && count == 0,
		  "four_xx_not_retried");
}

// 3. A 5xx is transient: three requests in all, then a network error.
static void case_five_xx_retries_thrice(void) {
	coverfetch_test_reset();
	coverfetch_test_set_status(500);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NETWORK && coverfetch_test_calls() == 3, "five_xx_retries_thrice");
}

// 4. With neither album nor artist there is nothing to ask for.
static void case_no_tags_short_circuits(void) {
	coverfetch_test_reset();
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("", "", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NO_TAGS && coverfetch_test_calls() == 0, "no_tags_short_circuits");
}

// 5. An album already marked not found is skipped without a request.
static void case_negative_cache_skips_request(void) {
	coverfetch_test_reset();
	char key[640];
	coverfetch_key(key, sizeof(key), "Abbey Road", "The Beatles");
	coverfetch_negative_add(key);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(coverfetch_negative_has(key) && r == COVERFETCH_NOT_FOUND && coverfetch_test_calls() == 0,
		  "negative_cache_skips_request");
}

// 6. A response that is not JSON is a network failure, not an empty result.
static void case_malformed_json_is_network(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body("{ this is not JSON");
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NETWORK && records == NULL && count == 0, "malformed_json_is_network");
}

// 7. No result path frees the array, and a successful one frees through the API.
static void case_search_frees_records(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body("{\"releases\":[{\"title\":\"no id\"},{\"title\":\"also no id\"}]}");
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	bool clean = r == COVERFETCH_NOT_FOUND && records == NULL && count == 0 &&
				 coverfetch_test_outstanding_records() == 0;

	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	clean = clean && r == COVERFETCH_OK && count == 2 && records != NULL &&
			strstr(records[1].url, "/rel-bbb/front-500") != NULL && records[0].size == 500;
	coverfetch_free(records, count);
	CHECK(clean && coverfetch_test_outstanding_records() == 0, "search_frees_records");
}

// ---------------------------------------------------------------------------
// Saving: a temp folder keeps every write out of the source tree. The dir hook
// in coverfetch.c sends all saves there, whatever track path the case passes.
// ---------------------------------------------------------------------------
#define SAVE_MAX_BYTES (4u * 1024u * 1024u)

static char save_dir[512];
static const unsigned char JPEG_BYTES[16] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD};
static const unsigned char EXISTING_BYTES[20] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
											   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};

static void save_dir_setup(void) {
	snprintf(save_dir, sizeof(save_dir), "build_host/coverfetch_XXXXXX");
	if (!mkdtemp(save_dir)) {
		printf("FAIL could not create save dir\n");
		exit(1);
	}
}

static void save_target(char *out, size_t size, const char *suffix) {
	snprintf(out, size, "%s/cover.jpg%s", save_dir, suffix);
}

static bool save_file_exists(void) {
	char path[600];
	save_target(path, sizeof(path), "");
	struct stat st;
	return stat(path, &st) == 0;
}

static bool save_part_exists(void) {
	const char *path = coverfetch_test_last_temp();
	struct stat st;
	return path && path[0] && stat(path, &st) == 0;
}

static bool save_file_equals(const unsigned char *want, size_t len) {
	char path[600];
	save_target(path, sizeof(path), "");
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char got[64];
	size_t n = fread(got, 1, sizeof(got), f);
	bool closed = fclose(f) == 0;
	return closed && n == len && memcmp(got, want, len) == 0;
}

static void save_put_existing(void) {
	char path[600];
	save_target(path, sizeof(path), "");
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(EXISTING_BYTES, 1, sizeof(EXISTING_BYTES), f) != sizeof(EXISTING_BYTES) || fclose(f) != 0) {
		printf("FAIL could not seed existing cover\n");
		exit(1);
	}
}

static void save_record(coverfetch_record_t *r) {
	memset(r, 0, sizeof(*r));
	snprintf(r->url, sizeof(r->url), "https://coverartarchive.org/release/rel-aaa/front-500");
	snprintf(r->source, sizeof(r->source), "Abbey Road");
	r->size = 500;
}

static void save_reset(void) {
	char path[600];
	save_target(path, sizeof(path), "");
	remove(path);
	save_target(path, sizeof(path), ".coverfetch.part");
	remove(path);
	coverfetch_test_reset();
	coverfetch_test_set_dir(save_dir);
}

// 8. A short GIF8 pile fails the length check; HTML fails the magic check.
static void case_save_rejects_non_image(void) {
	coverfetch_record_t r;
	save_record(&r);

	save_reset();
	coverfetch_test_set_download("GIF8junk", 8);
	bool short_rejected = !coverfetch_save("music/Album/track.mp3", "", &r, false) && !save_file_exists() &&
						  !save_part_exists();

	save_reset();
	const char html[] = "<html><body>no cover here</body></html>";
	coverfetch_test_set_download(html, sizeof(html) - 1);
	bool html_rejected = !coverfetch_save("music/Album/track.mp3", "", &r, false) && !save_file_exists() &&
						 !save_part_exists();
	CHECK(short_rejected && html_rejected, "save_rejects_non_image");
}

// 9. Four megabytes plus one is too big, even with valid JPEG magic.
static void case_save_rejects_oversized_body(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();

	unsigned char *big = malloc(SAVE_MAX_BYTES + 1);
	if (!big) {
		printf("FAIL could not allocate oversized body\n");
		exit(1);
	}
	big[0] = 0xFF;
	big[1] = 0xD8;
	big[2] = 0xFF;
	memset(big + 3, 0x5A, SAVE_MAX_BYTES - 2);
	coverfetch_test_set_download(big, SAVE_MAX_BYTES + 1);
	free(big);

	bool rejected = !coverfetch_save("music/Album/track.mp3", "", &r, false) && !save_file_exists() &&
					!save_part_exists();
	CHECK(rejected, "save_rejects_oversized_body");
}

// 10. With replace = false an existing cover is left byte for byte alone.
static void case_save_preserves_existing_art(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();
	save_put_existing();
	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	coverfetch_save("music/Album/track.mp3", "", &r, false);
	CHECK(save_file_equals(EXISTING_BYTES, sizeof(EXISTING_BYTES)), "save_preserves_existing_art");
}

// 11. With replace = true the new image wins and no temp file is left behind.
static void case_save_replaces_when_asked(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();
	save_put_existing();
	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	bool saved = coverfetch_save("music/Album/track.mp3", "", &r, true);
	CHECK(saved && save_file_equals(JPEG_BYTES, sizeof(JPEG_BYTES)) && !save_part_exists(),
		  "save_replaces_when_asked");
}

// 12. Two saves to one folder pick different temp paths, so neither can
// rename the other's half-written bytes into cover.jpg.
static void case_save_temp_paths_are_unique(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();

	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	bool first = coverfetch_save("music/Album/track.mp3", "", &r, true);
	char first_temp[1200];
	snprintf(first_temp, sizeof(first_temp), "%s", coverfetch_test_last_temp());
	bool first_clean = first && first_temp[0] && !save_part_exists();

	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	bool second = coverfetch_save("music/Album/track.mp3", "", &r, true);
	const char *second_temp = coverfetch_test_last_temp();

	CHECK(first_clean && second && second_temp[0] && strcmp(first_temp, second_temp) != 0 && !save_part_exists(),
		  "save_temp_paths_are_unique");
}

int main(void) {
	save_dir_setup();
	case_query_encodes_album_and_artist();
	case_four_xx_not_retried();
	case_five_xx_retries_thrice();
	case_no_tags_short_circuits();
	case_negative_cache_skips_request();
	case_malformed_json_is_network();
	case_search_frees_records();
	case_save_rejects_non_image();
	case_save_rejects_oversized_body();
	case_save_preserves_existing_art();
	case_save_replaces_when_asked();
	case_save_temp_paths_are_unique();

	char path[600];
	save_target(path, sizeof(path), "");
	remove(path);
	save_target(path, sizeof(path), ".coverfetch.part");
	remove(path);
	rmdir(save_dir);
	coverfetch_test_reset();
	printf("all coverfetch cases passed\n");
	return 0;
}
