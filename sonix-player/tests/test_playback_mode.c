// Host-side regression test for the playback-mode state machine and automatic
// queue advance. Only playlist.c is linked in; the library, cue, folder-order
// and config layers it normally talks to are stubbed below, so this needs
// neither LVGL nor the host audio libraries.
//
// The case that matters here: "stop after current track" is a music mode. It
// must not stop an audiobook's folder queue between parts, even though both use
// a non-ordered queue and the mode is persisted globally.

#include "src/system/playback/playlist.h"

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/library/cue.h"
#include "src/system/library/folderorder.h"
#include "src/system/library/library.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Stubs for the layers playlist.c talks to. None of them is reached by the
// queues built below (explicit path lists, no library handle, no cue sheets),
// but they have to exist for the link.
// ---------------------------------------------------------------------------

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

bool cue_is_sheet(const char *path) {
	(void)path;
	return false;
}

bool cue_parse(const char *cue_path, cue_sheet_t *out) {
	(void)cue_path;
	(void)out;
	return false;
}

void cue_virtual_path(const char *cue_path, int track_number, char *out, size_t size) {
	(void)cue_path;
	(void)track_number;
	if (size) {
		out[0] = '\0';
	}
}

bool cue_order_tracks(const char *a, const char *b, int *out) {
	(void)a;
	(void)b;
	(void)out;
	return false;
}

void folderorder_places(const char *folder, const char *const *names, size_t count, bool read_tags,
						folder_place_t *out) {
	(void)folder;
	(void)names;
	(void)read_tags;
	memset(out, 0, count * sizeof(*out));
}

int folderorder_cmp(const folder_place_t *a, const folder_place_t *b) {
	(void)a;
	(void)b;
	return 0;
}

bool has_extension(const char *name, const char *ext) {
	size_t n = strlen(name), e = strlen(ext);
	return n >= e && strcasecmp(name + n - e, ext) == 0;
}

library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)order;
	(void)desc;
	return NULL;
}

void library_index_close(library_index_t *ix) { (void)ix; }
int library_index_count(const library_index_t *ix) { (void)ix; return 0; }
bool library_index_stale(const library_index_t *ix) { (void)ix; return false; }

int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	(void)ix;
	(void)offset;
	(void)count;
	(void)cb;
	(void)user;
	return 0;
}

void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
}

int library_index_find_path(const library_index_t *ix, const char *path) {
	(void)ix;
	(void)path;
	return -1;
}

void library_queue_save_album(const char *album) { (void)album; }

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

static int failures;

#define CHECK(cond, msg)                                                                    \
	do {                                                                                    \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));              \
			failures++;                                                                  \
		}                                                                                   \
	} while (0)

static const char *tracks[] = {"/card/book/part1.mp3", "/card/book/part2.mp3", "/card/book/part3.mp3"};

static void test_cycle(void) {
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	CHECK(playlist_cycle_mode() == PLAYBACK_MODE_STOP_AFTER, "cycle NORMAL -> STOP_AFTER");
	CHECK(playlist_cycle_mode() == PLAYBACK_MODE_REPEAT_ALL, "cycle STOP_AFTER -> REPEAT_ALL");
}

static void test_stop_after_leaves_audiobook_queue_alone(void) {
	char next[512];

	playlist_set_mode(PLAYBACK_MODE_STOP_AFTER);
	// A folder queue (the shape a multi-part audiobook uses) is an ordinary,
	// non-ordered queue. Stop-after lives in device_state_advance_auto(), so the
	// playlist must still be able to step through the parts.
	playlist_load_paths(tracks, 3, 0);
	CHECK(!playlist_is_ordered(), "a folder-style queue is not ordered");

	CHECK(playlist_peek_auto(next, sizeof(next)), "peek of next audiobook part in stop-after mode");
	CHECK(strcmp(next, tracks[1]) == 0, "peek names part 2");
	CHECK(playlist_advance_auto(next, sizeof(next)), "advance to next audiobook part in stop-after mode");
	CHECK(strcmp(next, tracks[1]) == 0, "advance names part 2");
	CHECK(playlist_advance_auto(next, sizeof(next)), "advance to last audiobook part");
	CHECK(strcmp(next, tracks[2]) == 0, "advance names part 3");
	CHECK(!playlist_advance_auto(next, sizeof(next)), "the queue ends after its last part");
}

static void test_ordered_queue_advances_in_stop_after(void) {
	char next[512];

	playlist_set_mode(PLAYBACK_MODE_STOP_AFTER);
	playlist_load_paths_ordered(tracks, 3, 0);
	CHECK(playlist_is_ordered(), "podcast queue carries its own order");
	CHECK(playlist_peek_auto(next, sizeof(next)), "peek in an ordered queue in stop-after mode");
	CHECK(strcmp(next, tracks[1]) == 0, "ordered peek names episode 2");
	CHECK(playlist_advance_auto(next, sizeof(next)), "advance in an ordered queue in stop-after mode");
	CHECK(strcmp(next, tracks[1]) == 0, "ordered advance names episode 2");
}

static void test_normal_mode_stops_at_end(void) {
	char next[512];

	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(tracks, 3, 0);
	CHECK(playlist_advance_auto(next, sizeof(next)), "normal mode advances");
	CHECK(playlist_advance_auto(next, sizeof(next)), "normal mode advances again");
	CHECK(!playlist_advance_auto(next, sizeof(next)), "normal mode stops at the last track");
}

int main(void) {
	test_cycle();
	test_stop_after_leaves_audiobook_queue_alone();
	test_ordered_queue_advances_in_stop_after();
	test_normal_mode_stops_at_end();

	if (failures) {
		fprintf(stderr, "%d playback-mode check(s) failed\n", failures);
		return 1;
	}
	printf("playback-mode checks passed\n");
	return 0;
}
