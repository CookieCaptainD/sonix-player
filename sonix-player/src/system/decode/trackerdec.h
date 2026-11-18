#ifndef TRACKERDEC_H
#define TRACKERDEC_H

#include <stdbool.h>
#include <stdint.h>

// Tracker module decoding through libxmp (Protracker MOD, FastTracker XM,
// ScreamTracker S3M, Impulse IT, and everything else libxmp understands).
//
// A decoder always renders 44100 Hz, 16-bit signed, stereo interleaved PCM.
// Modules are played once: libxmp is told to stop at the first loop so a
// repeating module still reaches the end and the player can move on.

typedef struct trackerdec trackerdec_t;

// Opens a module from `path`. NULL when the file cannot be loaded or played.
trackerdec_t *trackerdec_open(const char *path);

void trackerdec_close(trackerdec_t *d);

// Fixed output format of the decoder.
int trackerdec_channels(const trackerdec_t *d);
int trackerdec_sample_rate(const trackerdec_t *d);

// Estimated length in PCM frames at the output rate, capped at 30 minutes.
// 0 when libxmp cannot estimate the duration.
uint64_t trackerdec_total_frames(const trackerdec_t *d);

// Writes at most `frames * 2` shorts of interleaved PCM into `buf`.
// Returns the number of frames actually written; 0 at the end of the module.
uint64_t trackerdec_read_s16(trackerdec_t *d, uint64_t frames, short *buf);

// Reloads the module and advances to `frame` (clamped to the end).
// Returns nonzero on success.
int trackerdec_seek(trackerdec_t *d, uint64_t frame);

// Module title from the metadata, valid while the decoder lives. Empty string
// when the module has no title.
const char *trackerdec_title(const trackerdec_t *d);

#endif /* TRACKERDEC_H */
