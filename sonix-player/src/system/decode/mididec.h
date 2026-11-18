#ifndef MIDIDEC_H
#define MIDIDEC_H

#include <stdbool.h>
#include <stdint.h>

// Standard MIDI File (type 0 and 1) playback through TinySoundFont.
//
// The decoder always renders 44100 Hz stereo interleaved s16, whatever the
// SoundFont's own sample rate is. Playback needs a SoundFont; metadata
// (title, duration) does not, so there are two ways to open a file.

typedef struct mididec mididec_t;

// Opens for playback. Returns NULL when the MIDI is invalid or when the
// SoundFont path is empty, missing, or not a SoundFont TinySoundFont accepts.
mididec_t *mididec_open(const char *path, const char *soundfont);

// Opens for metadata only: parses the SMF and computes title and duration
// without touching a SoundFont. mididec_read_s16() returns 0 on such a handle.
mididec_t *mididec_open_info(const char *path);

void mididec_close(mididec_t *d);

int mididec_channels(const mididec_t *d);
int mididec_sample_rate(const mididec_t *d);
uint64_t mididec_total_frames(const mididec_t *d);

// Renders up to 'frames' stereo frames (2 * frames shorts) into buf.
// Returns the number of frames actually written, 0 at the end of the file.
uint64_t mididec_read_s16(mididec_t *d, uint64_t frames, short *buf);

// Moves to 'frame', clamped to the end. Reapplies events without rendering.
// Returns 0 on success, -1 if the synth could not be rebuilt.
int mididec_seek(mididec_t *d, uint64_t frame);

// Track 0's name meta event, or an empty string when the file has none.
const char *mididec_title(const mididec_t *d);

#endif /* MIDIDEC_H */
