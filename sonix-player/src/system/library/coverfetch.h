#ifndef COVERFETCH_H
#define COVERFETCH_H

#include <stdbool.h>
#include <stddef.h>

// Cover art from MusicBrainz, served by the Cover Art Archive. A search asks
// MusicBrainz for releases matching an album and artist, and each hit becomes
// a record pointing at its 500 px front cover.
//
// Both calls block on the network: run them on a thread of their own, never on
// the interface thread. Only one request is in flight across all callers.

typedef enum {
	COVERFETCH_OK,			 // at least one release with a cover
	COVERFETCH_NOT_FOUND,	 // nothing for these tags, or a cached miss
	COVERFETCH_NO_TAGS,		 // no album and no artist to search with
	COVERFETCH_NETWORK,		 // no answer, or not one that could be read
	COVERFETCH_CANCELLED,	 // a batch was cancelled
} coverfetch_result_t;

typedef struct {
	char url[1024];	 // the Cover Art Archive front-500 URL
	char source[128]; // the release title, or "MusicBrainz"
	int size;		 // 500
} coverfetch_record_t;

// Searches by album and artist (title is kept for disambiguation in a later
// slice). On COVERFETCH_OK *records holds *count records; free with
// coverfetch_free(). Every other path leaves *records NULL and *count 0.
coverfetch_result_t coverfetch_search(const char *album, const char *artist, const char *title,
									  coverfetch_record_t **records, int *count);

void coverfetch_free(coverfetch_record_t *records, int count);

// Downloads a record's image and writes it as cover.jpg in dir_path, or in the
// folder of track_path when dir_path is empty. An existing cover.jpg is never
// overwritten unless replace is true. Returns true only when this call wrote a
// new file: a network error, a body over 4 MB, bytes that are not a supported
// image, or a filesystem error all leave the target untouched.
bool coverfetch_save(const char *track_path, const char *dir_path, const coverfetch_record_t *record, bool replace);

// Builds "query=release:<album> AND artist:<artist>" for the MusicBrainz ws/2
// release search, with every space encoded as %20 (values and separator).
void coverfetch_mb_query(char *out, size_t size, const char *album, const char *artist);

// The in-memory "not found" cache, keyed by coverfetch_key(). A hit lets a
// later batch skip an album that already came back empty.
bool coverfetch_negative_has(const char *key);
void coverfetch_negative_add(const char *key);
void coverfetch_negative_clear(void); // test hook: forget every miss

// A stable, case-folded key for an album and artist: "<album>|<artist>".
void coverfetch_key(char *out, size_t size, const char *album, const char *artist);

// Batch download: one album in each job, walked in order on a single worker.
// path is a representative track, dir the album folder (empty derives it from
// the track).
typedef struct {
	char album[256];
	char artist[256];
	char path[1024];
	char dir[1024];
} coverfetch_job_t;

// A progress post. done counts jobs finished (found plus not_found); cancelled
// is set once a batch has been asked to stop.
typedef struct {
	int total, done, found, not_found;
	bool cancelled;
} coverfetch_progress_t;

typedef void (*coverfetch_progress_cb)(const coverfetch_progress_t *p, void *user);

// How many albums in the library have no cover on their first track.
int coverfetch_batch_missing(void);

// Runs the jobs on a detached background thread, posting progress through cb
// after each one. The jobs are copied, so the caller's array may go away as
// soon as this returns. One request at a time, shared with the other callers.
void coverfetch_batch_run(const coverfetch_job_t *jobs, int count, coverfetch_progress_cb cb, void *user);

// Asks a running batch to stop before its next job. Safe to call from the
// progress callback. A cancel reaches only that run; starting another batch
// before it exits is refused.
void coverfetch_batch_cancel(void);

// True while a batch worker is still alive, including after a cancel while it
// finishes its current request. Check before showing progress for a new run.
bool coverfetch_batch_running(void);

#endif /* COVERFETCH_H */
