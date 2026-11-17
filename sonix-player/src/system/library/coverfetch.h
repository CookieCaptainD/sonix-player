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

#endif /* COVERFETCH_H */
