#include "trackerdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xmp.h>

// Rendered format. 44100 Hz is inside libxmp's supported range, and 0 as the
// start_player flag means "16-bit signed, stereo" (no XMP_FORMAT_MONO bit).
#define TRACKER_RATE 44100
#define TRACKER_CHANNELS 2
#define TRACKER_SCRATCH_FRAMES 4096
#define TRACKER_MAX_SECONDS (30 * 60)

struct trackerdec {
	xmp_context ctx;
	char *path;
	char title[XMP_NAME_SIZE];
	int channels;
	int sample_rate;
	uint64_t total_frames;
	uint64_t frames_read;
	short *scratch;
	bool eof;
};

// The fourth argument of xmp_play_buffer() is the loop limit. Passing 1 makes
// it report the end of the module as soon as the order list wraps around,
// instead of looping forever: a repeating module therefore finishes and the
// player can advance to the next track.
#define TRACKER_LOOP_LIMIT 1

static void teardown_context(trackerdec_t *d) {
	if (d->ctx) {
		xmp_end_player(d->ctx);
		xmp_release_module(d->ctx);
		xmp_free_context(d->ctx);
		d->ctx = NULL;
	}
}

static bool start_context(xmp_context ctx) {
	return xmp_start_player(ctx, TRACKER_RATE, 0) >= 0;
}

// Prefer libxmp's scanned duration; fall back to the module geometry when a
// loader does not provide one. Returns frames at the output rate.
static uint64_t estimate_total_frames(xmp_context ctx) {
	struct xmp_frame_info fi;
	memset(&fi, 0, sizeof(fi));
	xmp_get_frame_info(ctx, &fi);
	if (fi.total_time > 0) {
		return (uint64_t)fi.total_time * (uint64_t)TRACKER_RATE / 1000;
	}

	struct xmp_module_info mi;
	memset(&mi, 0, sizeof(mi));
	xmp_get_module_info(ctx, &mi);
	if (!mi.mod || mi.mod->len <= 0) {
		return 0;
	}

	int64_t rows = 0;
	for (int i = 0; i < mi.mod->len && i < XMP_MAX_MOD_LENGTH; i++) {
		int pat = mi.mod->xxo[i];
		if (pat >= 0 && pat < mi.mod->pat && mi.mod->xxp[pat]) {
			rows += mi.mod->xxp[pat]->rows;
		}
	}
	if (rows <= 0) {
		return 0;
	}

	int bpm = mi.mod->bpm > 0 ? mi.mod->bpm : 125;
	int speed = mi.mod->spd > 0 ? mi.mod->spd : 6;
	// libxmp's tick rate is 50 * bpm / 125 Hz; every row lasts `speed` ticks.
	double ticks_per_sec = 50.0 * (double)bpm / 125.0;
	double seconds = (double)rows * (double)speed / ticks_per_sec;
	if (seconds <= 0.0) {
		return 0;
	}
	return (uint64_t)(seconds * (double)TRACKER_RATE);
}

trackerdec_t *trackerdec_open(const char *path) {
	if (!path || !*path) {
		return NULL;
	}

	xmp_context ctx = xmp_create_context();
	if (!ctx) {
		return NULL;
	}
	if (xmp_load_module(ctx, path) < 0 || !start_context(ctx)) {
		xmp_release_module(ctx);
		xmp_free_context(ctx);
		return NULL;
	}

	trackerdec_t *d = calloc(1, sizeof(*d));
	if (!d) {
		xmp_end_player(ctx);
		xmp_release_module(ctx);
		xmp_free_context(ctx);
		return NULL;
	}

	size_t path_len = strlen(path);
	d->path = malloc(path_len + 1);
	if (!d->path) {
		teardown_context(d);
		free(d);
		return NULL;
	}
	memcpy(d->path, path, path_len + 1);

	d->scratch = malloc((size_t)TRACKER_SCRATCH_FRAMES * TRACKER_CHANNELS * sizeof(short));
	if (!d->scratch) {
		free(d->path);
		teardown_context(d);
		free(d);
		return NULL;
	}

	d->ctx = ctx;
	d->channels = TRACKER_CHANNELS;
	d->sample_rate = TRACKER_RATE;
	d->total_frames = estimate_total_frames(ctx);
	uint64_t limit = (uint64_t)TRACKER_MAX_SECONDS * (uint64_t)TRACKER_RATE;
	if (d->total_frames > limit) {
		d->total_frames = limit;
	}

	struct xmp_module_info mi;
	memset(&mi, 0, sizeof(mi));
	xmp_get_module_info(ctx, &mi);
	if (mi.mod && mi.mod->name[0]) {
		snprintf(d->title, sizeof(d->title), "%s", mi.mod->name);
		d->title[sizeof(d->title) - 1] = '\0';
	}

	return d;
}

void trackerdec_close(trackerdec_t *d) {
	if (!d) {
		return;
	}
	teardown_context(d);
	free(d->scratch);
	free(d->path);
	free(d);
}

int trackerdec_channels(const trackerdec_t *d) { return d ? d->channels : 0; }

int trackerdec_sample_rate(const trackerdec_t *d) { return d ? d->sample_rate : 0; }

uint64_t trackerdec_total_frames(const trackerdec_t *d) { return d ? d->total_frames : 0; }

uint64_t trackerdec_read_s16(trackerdec_t *d, uint64_t frames, short *buf) {
	if (!d || !buf || frames == 0 || d->eof) {
		return 0;
	}
	if (d->total_frames && d->frames_read >= d->total_frames) {
		d->eof = true;
		return 0;
	}

	uint64_t want = frames;
	if (d->total_frames) {
		uint64_t remaining = d->total_frames - d->frames_read;
		if (want > remaining) {
			want = remaining;
		}
	}
	if (want == 0) {
		d->eof = true;
		return 0;
	}

	uint64_t done = 0;
	while (done < want) {
		uint64_t chunk = want - done;
		if (chunk > TRACKER_SCRATCH_FRAMES) {
			chunk = TRACKER_SCRATCH_FRAMES;
		}
		int nbytes = (int)(chunk * TRACKER_CHANNELS * sizeof(short));
		int rc = xmp_play_buffer(d->ctx, d->scratch, nbytes, TRACKER_LOOP_LIMIT);
		if (rc < 0) {
			d->eof = true;
			break;
		}
		memcpy(buf + (size_t)done * TRACKER_CHANNELS, d->scratch, (size_t)nbytes);
		done += chunk;
	}

	d->frames_read += done;
	return done;
}

int trackerdec_seek(trackerdec_t *d, uint64_t frame) {
	if (!d || !d->ctx || !d->path) {
		return 0;
	}
	if (d->total_frames && frame > d->total_frames) {
		frame = d->total_frames;
	}

	xmp_context ctx = xmp_create_context();
	if (!ctx) {
		return 0;
	}
	if (xmp_load_module(ctx, d->path) < 0 || !start_context(ctx)) {
		xmp_release_module(ctx);
		xmp_free_context(ctx);
		return 0;
	}

	teardown_context(d);
	d->ctx = ctx;
	d->frames_read = 0;
	d->eof = false;

	// Advance by rendering and discarding frames. Cheap enough for a seek and
	// keeps the position in step with the loop-limited playback above.
	uint64_t remaining = frame;
	while (remaining > 0) {
		uint64_t chunk = remaining < TRACKER_SCRATCH_FRAMES ? remaining : TRACKER_SCRATCH_FRAMES;
		int nbytes = (int)(chunk * TRACKER_CHANNELS * sizeof(short));
		if (xmp_play_buffer(d->ctx, d->scratch, nbytes, TRACKER_LOOP_LIMIT) < 0) {
			d->eof = true;
			break;
		}
		remaining -= chunk;
	}
	d->frames_read = frame - remaining;
	return 1;
}

const char *trackerdec_title(const trackerdec_t *d) { return d ? d->title : ""; }
