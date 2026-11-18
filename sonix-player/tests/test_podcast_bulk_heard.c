// Host test for the bulk "mark as heard" state write (issue #149).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcaststate.h"

// main.o is left out of the link so the test can own main(). These five live
// there in the player; the state module never calls them.
void display_set_rotated(bool rotated) { (void)rotated; }
bool display_rotation_supported(void) { return false; }
void display_wake_begin(lv_display_t *d) { (void)d; }
void display_wake_end(lv_display_t *d) { (void)d; }
uint16_t *display_capture_frame(int *w, int *h) {
	if (w)
		*w = 0;
	if (h)
		*h = 0;
	return NULL;
}

// Declared here so this test compiles before the implementation exists.
bool podcaststate_set_played_many(const podcast_episode_t *const *episodes, int episode_count, const char *image, const char *const *paths, int path_count, bool played);

static int failures;

static void check(bool ok, const char *what) {
	if (ok) {
		printf("ok - %s\n", what);
	} else {
		printf("FAIL - %s\n", what);
		failures++;
	}
}

static void fill_episode(podcast_episode_t *e, long long id, const char *title) {
	memset(e, 0, sizeof(*e));
	e->id = id;
	e->feed_id = 4242;
	e->duration_secs = 3600;
	snprintf(e->title, sizeof(e->title), "%s", title);
	snprintf(e->feed_title, sizeof(e->feed_title), "Test show");
}

int main(void) {
	char dir[] = "/home/diogo/Downloads/Hiby/sonix-build/podcast149testXXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 2;
	}
	podcaststate_set_root(dir);

	podcast_episode_t a, b, c;
	fill_episode(&a, 16795090626LL, "Episode A");
	fill_episode(&b, 16795090627LL, "Episode B");
	fill_episode(&c, 16795090628LL, "Episode C");
	const podcast_episode_t *eps[] = {&a, &b, &c};

	bool ok = podcaststate_set_played_many(eps, 3, "https://img.example/cover.jpg", NULL, 0, true);
	check(ok, "batch of three feed episodes succeeds");

	podcaststate_t s;
	check(podcaststate_get_episode(a.id, &s) && s.played && s.position == 0.0, "episode A heard, position zero");
	check(strcmp(s.title, "Episode A") == 0, "episode A keeps its title");
	check(strcmp(s.image, "https://img.example/cover.jpg") == 0, "episode A keeps the feed image");
	check(podcaststate_get_episode(b.id, &s) && s.played, "episode B heard");
	check(podcaststate_get_episode(c.id, &s) && s.played, "episode C heard");

	// Repeating the batch must not duplicate rows or clear the metadata.
	ok = podcaststate_set_played_many(eps, 3, "https://img.example/cover.jpg", NULL, 0, true);
	check(ok && podcaststate_get_episode(b.id, &s) && s.played, "repeat is idempotent");

	// An empty batch is a no-op that still reports success.
	check(podcaststate_set_played_many(NULL, 0, NULL, NULL, 0, true), "empty batch succeeds");

	// Marking not heard clears the flag and leaves the metadata alone.
	check(podcaststate_set_played_many(eps, 1, NULL, NULL, 0, false), "unheard batch succeeds");
	check(podcaststate_get_episode(a.id, &s) && !s.played, "episode A not heard");
	check(strcmp(s.title, "Episode A") == 0, "episode A title survives unheard");

	podcaststate_set_root(NULL);
	printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
