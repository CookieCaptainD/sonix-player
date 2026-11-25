#ifndef PODCASTAUTO_H
#define PODCASTAUTO_H

#include <stdbool.h>
#include <stddef.h>

// Automatic podcast housekeeping, set per followed podcast (issue #74).
//
// The options live in device_config.ini under [podcast_auto]: one short entry
// per feed id, "enabled delete_played limit". They deliberately do NOT live in
// podcast.db: that database is owned by podcastsubs.c, which other work is
// changing, and a settings change here must not conflict with it.
//
// Everything here is safe to call from the GUI thread. The network sweep runs
// on its own thread, one download at a time through podcastdl.h, and never
// touches LVGL.

#define PODCAST_AUTO_MAX_PER_SYNC 5

typedef struct {
	bool enabled;		 // download new episodes when Wi-Fi comes up
	bool delete_played;	 // remove a downloaded episode once it is heard
	int limit;			 // most episodes kept on the card; 0 = no limit
} podcastauto_opts_t;

// All three are off by default: nobody loses a file or a download by updating.
void podcastauto_get(long long feed_id, podcastauto_opts_t *out);
void podcastauto_set(long long feed_id, const podcastauto_opts_t *opts);

// Called when the network becomes available. Does nothing when a sweep is
// already running, and returns immediately.
void podcastauto_sync(void);
bool podcastauto_syncing(void);

// Called when an episode has just been marked as heard, from both the player
// and the "Mark as heard" menu. Deletes its file only when its podcast asked
// for it and the file is one Sonix put in the Podcast folder. Files copied by
// hand and episodes in the temporary cache are left alone.
void podcastauto_on_played(const char *path, long long feed_id);

// Removes one kept episode (the file and its sidecars), keeping its heard
// state so "Continue listening" and the podcast page still know about it.
void podcastauto_delete_file(const char *path);

#endif /* PODCASTAUTO_H */
