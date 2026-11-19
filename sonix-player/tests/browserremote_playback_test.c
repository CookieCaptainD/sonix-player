/*
 * Playback-state isolation for the browser remote (src/system/net/browserremote.c).
 *
 * The promise under test: reading the remote's status, queue, library or
 * favourites leaves playback alone. Only an explicit control command may call
 * a playback function, and it calls exactly one. The player is injected as a
 * fake whose counters make an accidental play/pause or stop visible.
 */

#include <stdio.h>
#include <string.h>

#include "system/net/browserremote.h"

static int failures;

static void check(int condition, const char *what)
{
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

typedef struct {
	int state_calls;
	int queue_count_calls;
	int queue_item_calls;
	int library_calls;
	int fav_contains_calls;
	int fav_toggle_calls;
	int play_pause_calls;
	int next_calls;
	int prev_calls;
	int seek_calls;
	int volume_calls;
	int mode_calls;
	int play_path_calls;
	int play_index_calls;
	browserremote_state_t state;
	double last_seek;
	int last_volume;
	char last_path[BR_PATH_LEN];
	int last_index;
} fake_t;

static void f_state(browserremote_state_t *out, void *user)
{
	fake_t *f = user;
	f->state_calls++;
	*out = f->state;
}

static int f_queue_count(void *user)
{
	fake_t *f = user;
	f->queue_count_calls++;
	return f->state.queue_count;
}

static bool f_queue_item(int index, browserremote_item_t *out, void *user)
{
	fake_t *f = user;
	f->queue_item_calls++;
	if (index < 0 || index >= f->state.queue_count)
		return false;
	snprintf(out->path, sizeof(out->path), "/music/track-%d.flac", index);
	snprintf(out->title, sizeof(out->title), "Track %d", index);
	snprintf(out->artist, sizeof(out->artist), "Artist");
	return true;
}

static int f_library(const char *query, int offset, int count, browserremote_item_t *out, int max, void *user)
{
	fake_t *f = user;
	f->library_calls++;
	(void)query;
	(void)offset;
	int n = count < max ? count : max;
	for (int i = 0; i < n; i++) {
		snprintf(out[i].path, sizeof(out[i].path), "/music/lib-%d.flac", i);
		snprintf(out[i].title, sizeof(out[i].title), "Library %d", i);
		out[i].artist[0] = '\0';
	}
	return n;
}

static bool f_fav_contains(const char *path, void *user)
{
	fake_t *f = user;
	f->fav_contains_calls++;
	(void)path;
	return f->state.favourite;
}

static bool f_fav_toggle(const char *path, const char *title, const char *artist, void *user)
{
	fake_t *f = user;
	f->fav_toggle_calls++;
	snprintf(f->last_path, sizeof(f->last_path), "%s", path);
	(void)title;
	(void)artist;
	return !f->state.favourite;
}

static void f_play_pause(void *user) { ((fake_t *)user)->play_pause_calls++; }
static void f_next(void *user) { ((fake_t *)user)->next_calls++; }
static void f_prev(void *user) { ((fake_t *)user)->prev_calls++; }
static void f_seek(double seconds, void *user)
{
	fake_t *f = user;
	f->seek_calls++;
	f->last_seek = seconds;
}
static void f_volume(int percent, void *user)
{
	fake_t *f = user;
	f->volume_calls++;
	f->last_volume = percent;
}
static void f_mode(void *user) { ((fake_t *)user)->mode_calls++; }
static void f_play_path(const char *path, void *user)
{
	fake_t *f = user;
	f->play_path_calls++;
	snprintf(f->last_path, sizeof(f->last_path), "%s", path);
}
static void f_play_index(int index, void *user)
{
	fake_t *f = user;
	f->play_index_calls++;
	f->last_index = index;
}

static browserremote_ops_t ops = {
	.state = f_state,
	.queue_count = f_queue_count,
	.queue_item = f_queue_item,
	.library = f_library,
	.fav_contains = f_fav_contains,
	.fav_toggle = f_fav_toggle,
	.play_pause = f_play_pause,
	.next = f_next,
	.prev = f_prev,
	.seek = f_seek,
	.volume = f_volume,
	.mode = f_mode,
	.play_path = f_play_path,
	.play_index = f_play_index,
};

static int control_calls(const fake_t *f)
{
	return f->fav_toggle_calls + f->play_pause_calls + f->next_calls + f->prev_calls + f->seek_calls +
		   f->volume_calls + f->mode_calls + f->play_path_calls + f->play_index_calls;
}

static void setup(browserremote_ctx_t *ctx, fake_t *f)
{
	memset(f, 0, sizeof(*f));
	f->state.status = BR_STATUS_PLAYING;
	f->state.volume = 42;
	f->state.mode = BR_MODE_REPEAT_ALL;
	f->state.queue_count = 3;
	f->state.queue_index = 1;
	f->state.position = 12.5;
	f->state.duration = 200.0;
	f->state.favourite = true;
	strcpy(f->state.path, "/music/track-1.flac");
	strcpy(f->state.title, "Track 1");
	strcpy(f->state.artist, "Artist");

	ops.user = f;
	browserremote_init(ctx, &ops);
}

static void expect_read_only(browserremote_ctx_t *ctx, fake_t *f, browserremote_command_t cmd, const char *what)
{
	char out[4096];
	check(browserremote_execute(ctx, &(browserremote_cmd_t){.command = cmd}, out, sizeof(out)), what);
	check(out[0] == '{', "a read command answers with a JSON object");
	check(control_calls(f) == 0, "a read command never calls a playback function");
}

static void test_reads_leave_playback_alone(void)
{
	browserremote_ctx_t ctx;
	fake_t f;
	setup(&ctx, &f);

	expect_read_only(&ctx, &f, BR_CMD_STATUS, "status runs");
	check(f.state_calls > 0, "status reads the state");
	check(f.fav_contains_calls > 0, "status asks whether the track is a favourite");

	expect_read_only(&ctx, &f, BR_CMD_QUEUE, "queue runs");
	check(f.queue_count_calls > 0, "queue reads the count");
	check(f.queue_item_calls > 0, "queue reads its entries");

	expect_read_only(&ctx, &f, BR_CMD_LIBRARY, "library runs");
	check(f.library_calls > 0, "library browsing reads the library");

	browserremote_cmd_t fav = {.command = BR_CMD_FAVOURITES};
	char out[4096];
	check(browserremote_execute(&ctx, &fav, out, sizeof(out)), "favourites runs");
	check(control_calls(&f) == 0, "listing favourites never calls a playback function");

	/* The snapshot the remote reports is still the one the fake owns: no read
	 * request changed a single playback field. */
	check(f.state.status == BR_STATUS_PLAYING && f.state.volume == 42 && f.state.position == 12.5,
		  "reading state leaves playback fields untouched");
}

static void test_control_calls_exactly_one_op(void)
{
	browserremote_ctx_t ctx;
	fake_t f;
	char out[256];
	browserremote_cmd_t cmd = {0};

	setup(&ctx, &f);
	cmd.command = BR_CMD_PLAY_PAUSE;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.play_pause_calls == 1 && control_calls(&f) == 1, "play/pause calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_NEXT;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.next_calls == 1 && control_calls(&f) == 1, "next calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_PREV;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.prev_calls == 1 && control_calls(&f) == 1, "previous calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_SEEK;
	cmd.seconds = 75.5;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.seek_calls == 1 && control_calls(&f) == 1 && f.last_seek == 75.5, "seek calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_VOLUME;
	cmd.percent = 73;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.volume_calls == 1 && control_calls(&f) == 1 && f.last_volume == 73, "volume calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_MODE;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.mode_calls == 1 && control_calls(&f) == 1, "play mode calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_TOGGLE_FAV;
	strcpy(cmd.path, "/music/track-1.flac");
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.fav_toggle_calls == 1 && control_calls(&f) == 1, "favourite toggle calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_PLAY_PATH;
	strcpy(cmd.path, "/music/track-2.flac");
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.play_path_calls == 1 && control_calls(&f) == 1 && strcmp(f.last_path, "/music/track-2.flac") == 0,
		  "play by path calls exactly one op");

	setup(&ctx, &f);
	cmd.command = BR_CMD_PLAY_INDEX;
	cmd.index = 2;
	browserremote_execute(&ctx, &cmd, out, sizeof(out));
	check(f.play_index_calls == 1 && control_calls(&f) == 1 && f.last_index == 2, "play by index calls exactly one op");
}

static void test_no_ops_table(void)
{
	browserremote_ctx_t ctx;
	char out[128];
	browserremote_init(&ctx, NULL);
	check(!browserremote_execute(&ctx, &(browserremote_cmd_t){.command = BR_CMD_STATUS}, out, sizeof(out)),
		  "a command with no ops table fails");
}

int main(void)
{
	test_reads_leave_playback_alone();
	test_control_calls_exactly_one_op();
	test_no_ops_table();

	if (failures == 0) {
		printf("browserremote_playback_test: all checks passed\n");
		return 0;
	}
	printf("browserremote_playback_test: %d failure(s)\n", failures);
	return 1;
}
