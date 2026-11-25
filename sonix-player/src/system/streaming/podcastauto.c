#include "podcastauto.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/podcastsubs.h"

// The options are one line per followed podcast in device_config.ini:
//
//   [podcast_auto]
//   9123456789012345 = 1 1 10
//
// config.h limits keys to 32 characters and values to 128; a 64-bit feed id
// and three small numbers fit both with room to spare. Keeping this out of
// podcast.db is the point: podcastsubs.c owns that database and other work is
// changing its schema, so a settings patch here must not touch it.

#define AUTO_SECTION "podcast_auto"
#define AUTO_EPISODES 32 // one page of a feed per sync
#define AUTO_BUSY_WAIT_MS 250
#define AUTO_BUSY_TRIES 2400 // ten minutes: an episode is an hour long at most

void podcastauto_get(long long feed_id, podcastauto_opts_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (feed_id <= 0) {
		return;
	}
	char key[32];
	char value[64];
	snprintf(key, sizeof(key), "%lld", feed_id);
	const char *stored = config_get(AUTO_SECTION, key, "");
	if (!stored) {
		return;
	}
	snprintf(value, sizeof(value), "%s", stored);
	int enabled = 0;
	int delete_played = 0;
	int limit = 0;
	if (sscanf(value, "%d %d %d", &enabled, &delete_played, &limit) == 3) {
		out->enabled = enabled != 0;
		out->delete_played = delete_played != 0;
		out->limit = limit > 0 ? limit : 0;
		if (out->limit > PODCASTSUBS_MAX) {
			out->limit = PODCASTSUBS_MAX;
		}
	}
}

void podcastauto_set(long long feed_id, const podcastauto_opts_t *opts) {
	if (feed_id <= 0 || !opts) {
		return;
	}
	char key[32];
	char value[64];
	snprintf(key, sizeof(key), "%lld", feed_id);
	snprintf(value, sizeof(value), "%d %d %d", opts->enabled ? 1 : 0, opts->delete_played ? 1 : 0,
			 opts->limit > 0 ? opts->limit : 0);
	config_set(AUTO_SECTION, key, value);
	config_save(); // a setting lost at the next power-off is no setting at all
}

// ---------------------------------------------------------------------------
// deleting a kept episode
// ---------------------------------------------------------------------------

void podcastauto_delete_file(const char *path) {
	if (!path || !path[0] || !podcastdl_owns(path)) {
		return;
	}
	// The state row stays: an episode of the catalogue keeps its place in
	// "Continue listening" and its heard mark when its file goes away.
	char side[900];
	remove(path);
	snprintf(side, sizeof(side), "%s.tags", path);
	remove(side);
	snprintf(side, sizeof(side), "%s.jpg", path);
	remove(side);
	snprintf(side, sizeof(side), "%s.png", path);
	remove(side);
	sync();
	printf("podcastauto: removed %s\n", path);
}

void podcastauto_on_played(const char *path, long long feed_id) {
	if (!path || !path[0] || feed_id <= 0 || !podcastdl_owns(path)) {
		return;
	}
	podcastauto_opts_t opts;
	podcastauto_get(feed_id, &opts);
	if (opts.delete_played) {
		podcastauto_delete_file(path);
	}
}

// ---------------------------------------------------------------------------
// the sweep
// ---------------------------------------------------------------------------

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static bool syncing;

bool podcastauto_syncing(void) {
	pthread_mutex_lock(&sync_lock);
	bool running = syncing;
	pthread_mutex_unlock(&sync_lock);
	return running;
}

typedef struct {
	char *path;
	bool played;
	long long touched;
} kept_t;

typedef struct {
	kept_t *items;
	int count;
	int capacity;
} kept_list_t;

static void kept_free(kept_list_t *list) {
	for (int i = 0; i < list->count; i++) {
		free(list->items[i].path);
	}
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static void kept_add(kept_list_t *list, const char *path) {
	if (list->count == list->capacity) {
		int grown = list->capacity ? list->capacity * 2 : 32;
		kept_t *bigger = realloc(list->items, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return;
		}
		list->items = bigger;
		list->capacity = grown;
	}
	char *copy = strdup(path);
	if (!copy) {
		return;
	}
	list->items[list->count].path = copy;
	list->items[list->count].played = false;
	list->items[list->count].touched = 0;
	list->count++;
}

static bool kept_file(const char *name) {
	if (!name || !name[0] || name[0] == '.') {
		return false;
	}
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	return strcasecmp(dot, ".tags") != 0 && strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".png") != 0;
}

static void kept_read_folder(const char *folder, kept_list_t *list) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (!kept_file(de->d_name)) {
			continue;
		}
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", folder, de->d_name);
		kept_add(list, path);
	}
	closedir(dir);
}

static void kept_cb(const podcaststate_t *state, void *user) {
	kept_list_t *list = user;
	if (!state->path[0]) {
		return;
	}
	for (int i = 0; i < list->count; i++) {
		if (strcmp(list->items[i].path, state->path) == 0) {
			list->items[i].played = state->played;
			list->items[i].touched = state->touched;
			return;
		}
	}
}

static int kept_cmp(const void *a, const void *b) {
	const kept_t *x = a;
	const kept_t *y = b;
	// Heard episodes go first; among those, the oldest; a file with no state
	// (copied in by hand) goes last, since nothing says it is safe to drop.
	if (x->played != y->played) {
		return x->played ? -1 : 1;
	}
	if (x->touched != y->touched) {
		return x->touched < y->touched ? -1 : 1;
	}
	return strcmp(x->path, y->path);
}

static void kept_trim(kept_list_t *list, int limit) {
	if (limit <= 0 || list->count <= limit) {
		return;
	}
	// The episode playing now carries PODCASTSTATE_TOUCHED_NOW. Removing that
	// file under the decoder is not worth the risk, so it is left for later.
	qsort(list->items, (size_t)list->count, sizeof(*list->items), kept_cmp);
	int removed = 0;
	for (int i = 0; i < list->count && list->count - removed > limit; i++) {
		kept_t *item = &list->items[i];
		if (item->touched == PODCASTSTATE_TOUCHED_NOW) {
			continue;
		}
		podcastauto_delete_file(item->path);
		free(item->path);
		item->path = NULL;
		removed++;
	}
	int kept = 0;
	for (int i = 0; i < list->count; i++) {
		if (!list->items[i].path) {
			continue;
		}
		if (kept != i) {
			list->items[kept] = list->items[i];
		}
		kept++;
	}
	list->count = kept;
}

static void wait_for_download(void) {
	for (int i = 0; i < AUTO_BUSY_TRIES && podcastdl_busy(); i++) {
		usleep(AUTO_BUSY_WAIT_MS * 1000);
	}
}

static void feed_folder(long long feed_id, char *out, size_t size, char *title, size_t title_size) {
	podcast_feed_t feeds[PODCASTSUBS_MAX];
	podcast_feed_t feed;
	memset(&feed, 0, sizeof(feed));
	int n = podcastsubs_list(feeds, PODCASTSUBS_MAX);
	for (int i = 0; i < n; i++) {
		if (feeds[i].id == feed_id) {
			feed = feeds[i];
			break;
		}
	}
	char fallback[32];
	snprintf(fallback, sizeof(fallback), "%lld", feed_id);
	snprintf(title, title_size, "%s", feed.title[0] ? feed.title : fallback);
	char name[160];
	podcastdl_safe_name(title, "Podcast", name, sizeof(name));
	snprintf(out, size, "%s/%s", podcastdl_root(), name);
}

static void sync_feed(const podcast_feed_t *feed) {
	podcastauto_opts_t opts;
	podcastauto_get(feed->id, &opts);
	if (!opts.enabled) {
		return;
	}

	char folder[700];
	char title[PODCAST_TITLE_MAX];
	feed_folder(feed->id, folder, sizeof(folder), title, sizeof(title));

	kept_list_t kept = {0};
	kept_read_folder(folder, &kept);
	podcaststate_in_folder(folder, kept_cb, &kept);
	kept_trim(&kept, opts.limit);

	int slots = opts.limit > 0 ? opts.limit - kept.count : PODCAST_AUTO_MAX_PER_SYNC;
	if (slots <= 0) {
		kept_free(&kept);
		return;
	}

	podcast_episode_t episodes[AUTO_EPISODES];
	int n = podcast_episodes(feed->id, episodes, AUTO_EPISODES);
	int added = 0;
	for (int i = 0; i < n && added < slots && added < PODCAST_AUTO_MAX_PER_SYNC; i++) {
		const podcast_episode_t *e = &episodes[i];
		if (!e->enclosure[0] || podcastdl_exists(e, title)) {
			continue;
		}
		podcaststate_t state;
		if (podcaststate_get_episode(e->id, &state) && state.played) {
			continue;
		}
		wait_for_download();
		if (podcastdl_busy()) {
			break; // somebody else is downloading; leave the radio to them
		}
		if (!podcastdl_start(e, title, feed->author, feed->image)) {
			continue;
		}
		wait_for_download();
		podcastdl_state_t result = podcastdl_state(NULL, NULL);
		podcastdl_acknowledge();
		if (result == PODCASTDL_DONE) {
			added++;
			if (opts.limit > 0) {
				kept_free(&kept);
				memset(&kept, 0, sizeof(kept));
				kept_read_folder(folder, &kept);
				podcaststate_in_folder(folder, kept_cb, &kept);
				kept_trim(&kept, opts.limit);
				slots = opts.limit - kept.count;
			}
		}
	}
	kept_free(&kept);
}

static void *sync_main(void *arg) {
	(void)arg;
	thread_be_background("podcast auto");
	podcast_feed_t feeds[PODCASTSUBS_MAX];
	int n = podcastsubs_list(feeds, PODCASTSUBS_MAX);
	for (int i = 0; i < n; i++) {
		sync_feed(&feeds[i]);
	}
	pthread_mutex_lock(&sync_lock);
	syncing = false;
	pthread_mutex_unlock(&sync_lock);
	return NULL;
}

void podcastauto_sync(void) {
	pthread_mutex_lock(&sync_lock);
	if (syncing) {
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	syncing = true;
	pthread_mutex_unlock(&sync_lock);

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	if (pthread_create(&thread, &attr, sync_main, NULL) != 0) {
		pthread_mutex_lock(&sync_lock);
		syncing = false;
		pthread_mutex_unlock(&sync_lock);
	}
	pthread_attr_destroy(&attr);
}
