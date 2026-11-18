#include "mididec.h"

#include "third_party/tsf/tsf.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Optional entries: a bundled copy of TinySoundFont may predate them. Weak so
// the decoder builds either way. Without tsf_program_change the GM program is
// resolved to a preset index with tsf_get_presetindex at note-on; without
// tsf_reset, seek rebuilds the synth from the SoundFont path instead.
extern void tsf_program_change(tsf *f, int program, int bank) __attribute__((weak));
extern int tsf_get_presetindex(const tsf *f, int bank, int preset_number) __attribute__((weak));
extern void tsf_reset(tsf *f) __attribute__((weak));

#define MIDIDEC_RATE 44100
#define DEFAULT_TEMPO_US 500000u // 120 BPM

typedef enum {
	EV_NOTE_ON,
	EV_NOTE_OFF,
	EV_PROGRAM,
	EV_CONTROL,
	EV_TEMPO
} event_kind;

// One SMF event, merged across tracks and sorted by absolute tick. 'order'
// keeps the original parse order for events on the same tick (program change
// and controllers before the notes that follow them).
typedef struct {
	uint64_t tick;
	uint64_t frame;
	uint32_t order;
	uint8_t kind;
	uint8_t channel;
	uint8_t a; // note, program, or controller number
	uint8_t b; // velocity or controller value
	uint32_t tempo_us;
} event;

// A tempo change: seconds per tick from 'tick' onwards, until the next mark.
typedef struct {
	uint64_t tick;
	double sec_per_tick;
} tempo_mark;

struct mididec {
	event *events;
	size_t count;
	size_t cap;
	tempo_mark *tempos;
	size_t tempo_count;
	size_t tempo_cap;
	uint32_t order;
	uint64_t last_tick;
	uint64_t total_frames;
	char title[256];
	tsf *synth;
	char *soundfont;
	uint64_t pos; // frames rendered so far
	size_t next; // first event not yet applied

	// Channel state, replayed on seek. TinySoundFont's note_on takes a preset
	// index; the bundled program_change wrapper maps GM programs onto it.
	unsigned char volume[16];
	unsigned char pan[16];
	unsigned char program[16];
	unsigned char bank[16];
	int preset[16]; // preset index handed to tsf_note_on / tsf_note_off
	unsigned char active[16][16]; // 128 note bits per channel
};

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static uint16_t be16(const unsigned char *p) {
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool read_varlen(const unsigned char **pp, const unsigned char *end, uint32_t *out) {
	uint32_t value = 0;
	const unsigned char *p = *pp;
	for (int i = 0; i < 4; i++) {
		if (p >= end) {
			return false;
		}
		unsigned char c = *p++;
		value = (value << 7) | (c & 0x7f);
		if (!(c & 0x80)) {
			*pp = p;
			*out = value;
			return true;
		}
	}
	return false;
}

static unsigned char *read_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long n = ftell(f);
	if (n < 14 || n > 64 * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	rewind(f);

	unsigned char *data = (unsigned char *)malloc((size_t)n);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if (fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return data;
}

static bool grow_events(mididec_t *d) {
	if (d->cap > d->count) {
		return true;
	}
	size_t cap = d->cap ? d->cap * 2 : 64;
	event *p = (event *)realloc(d->events, cap * sizeof(*p));
	if (!p) {
		return false;
	}
	d->events = p;
	d->cap = cap;
	return true;
}

static bool add_event(mididec_t *d, uint64_t tick, uint8_t kind, uint8_t channel, uint8_t a, uint8_t b, uint32_t tempo_us) {
	if (!grow_events(d)) {
		return false;
	}
	event *e = &d->events[d->count++];
	e->tick = tick;
	e->frame = 0;
	e->order = d->order++;
	e->kind = kind;
	e->channel = channel;
	e->a = a;
	e->b = b;
	e->tempo_us = tempo_us;
	return true;
}

static bool add_tempo(mididec_t *d, uint64_t tick, double sec_per_tick) {
	if (d->tempo_cap > d->tempo_count) {
		// nothing to grow
	} else {
		size_t cap = d->tempo_cap ? d->tempo_cap * 2 : 16;
		tempo_mark *p = (tempo_mark *)realloc(d->tempos, cap * sizeof(*p));
		if (!p) {
			return false;
		}
		d->tempos = p;
		d->tempo_cap = cap;
	}
	tempo_mark *m = &d->tempos[d->tempo_count++];
	m->tick = tick;
	m->sec_per_tick = sec_per_tick;
	return true;
}

static int compare_events(const void *pa, const void *pb) {
	const event *a = (const event *)pa;
	const event *b = (const event *)pb;
	if (a->tick < b->tick) {
		return -1;
	}
	if (a->tick > b->tick) {
		return 1;
	}
	if (a->order < b->order) {
		return -1;
	}
	if (a->order > b->order) {
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// SMF parsing
// ---------------------------------------------------------------------------

static bool parse_track(mididec_t *d, const unsigned char *p, const unsigned char *end, bool first_track) {
	uint64_t tick = 0;
	uint8_t status = 0;

	while (p < end) {
		uint32_t delta;
		if (!read_varlen(&p, end, &delta)) {
			return false;
		}
		tick += delta;
		if (p >= end) {
			return false;
		}

		uint8_t lead = *p;
		if (lead & 0x80) {
			status = lead;
			p++;
		} else if (!status) {
			return false; // running status before any status byte
		}

		if (status == 0xFF) {
			if (p >= end) {
				return false;
			}
			uint8_t type = *p++;
			uint32_t len;
			if (!read_varlen(&p, end, &len) || len > (uint32_t)(end - p)) {
				return false;
			}
			if (type == 0x51 && len >= 3) {
				uint32_t us = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
				if (!add_event(d, tick, EV_TEMPO, 0, 0, 0, us)) {
					return false;
				}
			} else if (type == 0x03 && first_track && len > 0) {
				size_t n = len < sizeof(d->title) - 1 ? len : sizeof(d->title) - 1;
				memcpy(d->title, p, n);
				d->title[n] = '\0';
			}
			p += len;
			status = 0; // system messages cancel running status
		} else if (status == 0xF0 || status == 0xF7) {
			uint32_t len;
			if (!read_varlen(&p, end, &len) || len > (uint32_t)(end - p)) {
				return false;
			}
			p += len;
			status = 0;
		} else if (status >= 0x80 && status <= 0xEF) {
			uint8_t high = status & 0xF0;
			uint8_t channel = status & 0x0F;
			if (p >= end) {
				return false;
			}
			uint8_t a = *p++;
			uint8_t b = 0;
			if (high != 0xC0 && high != 0xD0) {
				if (p >= end) {
					return false;
				}
				b = *p++;
			}

			switch (high) {
			case 0x80:
				if (!add_event(d, tick, EV_NOTE_OFF, channel, a, 0, 0)) {
					return false;
				}
				break;
			case 0x90:
				// Note-on at velocity zero is a note-off in practice.
				if (!add_event(d, tick, b ? EV_NOTE_ON : EV_NOTE_OFF, channel, a, b, 0)) {
					return false;
				}
				break;
			case 0xB0:
				if (!add_event(d, tick, EV_CONTROL, channel, a, b, 0)) {
					return false;
				}
				break;
			case 0xC0:
				if (!add_event(d, tick, EV_PROGRAM, channel, a, 0, 0)) {
					return false;
				}
				break;
			default:
				// aftertouch and pitch bend are not used by this decoder
				break;
			}
		} else {
			return false; // system realtime bytes are not expected inside a track
		}
	}
	return true;
}

static double default_sec_per_tick(uint16_t division) {
	if (division & 0x8000) {
		// SMPTE division: frames per second (negative byte) * ticks per frame.
		int fps = 256 - (division >> 8);
		int ticks = division & 0xFF;
		return 1.0 / ((double)fps * (double)ticks);
	}
	// Metrical division: quarter note = 500000 us at the default 120 BPM.
	return 0.5 / (double)division;
}

static uint64_t tick_to_frame(const mididec_t *d, uint64_t tick) {
	double seconds = 0.0;
	uint64_t previous = d->tempo_count ? d->tempos[0].tick : 0;
	// The rate in force is the one set by the previous mark: the interval up
	// to a change must be charged at the old rate, not the new one.
	double rate = d->tempo_count ? d->tempos[0].sec_per_tick : 0.0;
	for (size_t i = 0; i < d->tempo_count; i++) {
		const tempo_mark *m = &d->tempos[i];
		if (m->tick > tick) {
			break;
		}
		seconds += (double)(m->tick - previous) * rate;
		previous = m->tick;
		rate = m->sec_per_tick;
	}
	seconds += (double)(tick - previous) * rate;
	if (seconds <= 0.0) {
		return 0;
	}
	double frames = seconds * (double)MIDIDEC_RATE + 0.5;
	if (frames > (double)ULLONG_MAX) {
		return ULLONG_MAX;
	}
	return (uint64_t)frames;
}

static bool parse_midi(mididec_t *d, const char *path) {
	size_t size = 0;
	unsigned char *data = read_file(path, &size);
	if (!data) {
		return false;
	}

	const unsigned char *p = data;
	const unsigned char *end = data + size;
	uint16_t division = 0;
	bool ok = false;

	do {
		if ((size_t)(end - p) < 14 || memcmp(p, "MThd", 4) != 0) {
			break;
		}
		uint32_t header_len = be32(p + 4);
		if (header_len < 6 || header_len > (uint32_t)(end - p - 8)) {
			break;
		}
		uint16_t format = be16(p + 8);
		uint16_t tracks = be16(p + 10);
		division = be16(p + 12);
		if ((format != 0 && format != 1) || tracks == 0 || division == 0) {
			break;
		}
		p += 8 + header_len;

		bool all_tracks = true;
		for (uint16_t i = 0; i < tracks; i++) {
			if ((size_t)(end - p) < 8 || memcmp(p, "MTrk", 4) != 0) {
				all_tracks = false;
				break;
			}
			uint32_t track_len = be32(p + 4);
			if (track_len > (uint32_t)(end - p - 8)) {
				all_tracks = false;
				break;
			}
			const unsigned char *track = p + 8;
			if (!parse_track(d, track, track + track_len, i == 0)) {
				all_tracks = false;
				break;
			}
			p = track + track_len;
		}
		if (!all_tracks) {
			break;
		}
		ok = true;
	} while (0);

	free(data);
	if (!ok) {
		return false;
	}

	if (d->count > 1) {
		qsort(d->events, d->count, sizeof(*d->events), compare_events);
	}

	// Tempo map: the default at tick zero, then every meta tempo change. Two
	// changes on one tick leave the later one in force via tick_to_frame().
	double base = default_sec_per_tick(division);
	if (!add_tempo(d, 0, base)) {
		return false;
	}
	for (size_t i = 0; i < d->count; i++) {
		const event *e = &d->events[i];
		if (e->kind == EV_TEMPO) {
			// In SMPTE division the ticks per second is fixed by the header,
			// so a tempo meta event carries no conversion information.
			double rate = (division & 0x8000) ? base : ((double)e->tempo_us / 1000000.0) / (double)division;
			if (!add_tempo(d, e->tick, rate)) {
				return false;
			}
		}
		if (e->tick > d->last_tick) {
			d->last_tick = e->tick;
		}
	}

	d->total_frames = tick_to_frame(d, d->last_tick);
	for (size_t i = 0; i < d->count; i++) {
		d->events[i].frame = tick_to_frame(d, d->events[i].tick);
	}
	return true;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void mididec_close(mididec_t *d) {
	if (!d) {
		return;
	}
	if (d->synth) {
		tsf_close(d->synth);
	}
	free(d->soundfont);
	free(d->events);
	free(d->tempos);
	free(d);
}

// ---------------------------------------------------------------------------
// playback
// ---------------------------------------------------------------------------

// Upstream TinySoundFont's note_on takes a preset *index*, not a GM program
// number. A bundled wrapper may provide tsf_program_change instead; when it
// does not, ask the synth to map bank/program onto its preset table. If the
// synth's lookup is absent too, fall back to the program number itself.
static int resolve_preset(const mididec_t *d, int ch) {
	int program = d->program[ch];
	if (!tsf_program_change && tsf_get_presetindex && d->synth) {
		int index = tsf_get_presetindex(d->synth, d->bank[ch], program);
		if (index >= 0) {
			return index;
		}
	}
	return program;
}

static void reset_channel_state(mididec_t *d) {
	for (int ch = 0; ch < 16; ch++) {
		d->volume[ch] = 100; // GM default
		d->pan[ch] = 64;
		d->program[ch] = 0;
		d->bank[ch] = 0;
		d->preset[ch] = resolve_preset(d, ch);
		memset(d->active[ch], 0, sizeof(d->active[ch]));
	}
}

static void apply_event(mididec_t *d, const event *e) {
	int ch = e->channel;
	if (ch < 0 || ch > 15) {
		return;
	}

	switch (e->kind) {
	case EV_NOTE_ON: {
		int key = e->a & 0x7F;
		float velocity = (float)(e->b & 0x7F) / 127.0f * ((float)d->volume[ch] / 127.0f);
		tsf_note_on(d->synth, d->preset[ch], key, velocity);
		d->active[ch][key >> 3] |= (unsigned char)(1u << (key & 7));
		break;
	}
	case EV_NOTE_OFF: {
		int key = e->a & 0x7F;
		tsf_note_off(d->synth, d->preset[ch], key);
		d->active[ch][key >> 3] &= (unsigned char)~(1u << (key & 7));
		break;
	}
	case EV_PROGRAM:
		d->program[ch] = e->a & 0x7F;
		d->preset[ch] = resolve_preset(d, ch);
		if (tsf_program_change) {
			tsf_program_change(d->synth, d->program[ch], d->bank[ch]);
		}
		break;
	case EV_CONTROL:
		if (e->a == 7) {
			d->volume[ch] = e->b;
		} else if (e->a == 10) {
			// Parsed for completeness; the small tsf API used here has no
			// per-channel pan call, so pan is kept as state only.
			d->pan[ch] = e->b;
		} else if (e->a == 0) {
			d->bank[ch] = e->b; // bank select MSB
			d->preset[ch] = resolve_preset(d, ch);
		} else if (e->a == 32) {
			d->bank[ch] = e->b; // bank select LSB (GM uses the MSB)
			d->preset[ch] = resolve_preset(d, ch);
		} else if (e->a == 123) {
			// All notes off: TinySoundFont has no per-channel variant in the
			// API used here, so turn off the notes this decoder started.
			for (int key = 0; key < 128; key++) {
				if (d->active[ch][key >> 3] & (1u << (key & 7))) {
					tsf_note_off(d->synth, d->preset[ch], key);
				}
			}
			memset(d->active[ch], 0, sizeof(d->active[ch]));
		}
		break;
	case EV_TEMPO:
		// Already baked into every event's frame when the file was parsed.
		break;
	}
}

static mididec_t *open_common(const char *path, const char *soundfont) {
	if (!path || !path[0]) {
		return NULL;
	}
	mididec_t *d = (mididec_t *)calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}
	if (!parse_midi(d, path)) {
		mididec_close(d);
		return NULL;
	}
	if (soundfont && soundfont[0]) {
		// Keep the "no SoundFont" error visible: an empty or missing path is
		// a hard failure rather than a silent fall back to silence.
		FILE *f = fopen(soundfont, "rb");
		if (!f) {
			mididec_close(d);
			return NULL;
		}
		fclose(f);

		d->synth = tsf_load_filename(soundfont);
		if (!d->synth) {
			mididec_close(d);
			return NULL;
		}
		size_t n = strlen(soundfont);
		d->soundfont = (char *)malloc(n + 1);
		if (!d->soundfont) {
			mididec_close(d);
			return NULL;
		}
		memcpy(d->soundfont, soundfont, n + 1);
		tsf_set_output(d->synth, TSF_STEREO_INTERLEAVED, MIDIDEC_RATE, 0.0f);
	}
	reset_channel_state(d);
	return d;
}

mididec_t *mididec_open(const char *path, const char *soundfont) {
	if (!soundfont || !soundfont[0]) {
		return NULL;
	}
	return open_common(path, soundfont);
}

mididec_t *mididec_open_info(const char *path) {
	return open_common(path, NULL);
}

uint64_t mididec_read_s16(mididec_t *d, uint64_t frames, short *buf) {
	if (!d || !buf || !frames || !d->synth || d->pos >= d->total_frames) {
		return 0;
	}

	uint64_t want = frames;
	if (want > d->total_frames - d->pos) {
		want = d->total_frames - d->pos;
	}

	uint64_t done = 0;
	while (done < want) {
		uint64_t chunk_end = d->pos + (want - done);
		bool applied = false;

		// Events are honoured at their exact frame, inside the read block.
		while (d->next < d->count && d->events[d->next].frame <= chunk_end) {
			uint64_t before = d->events[d->next].frame > d->pos ? d->events[d->next].frame - d->pos : 0;
			if (before > 0) {
				if (before > (uint64_t)INT_MAX) {
					before = (uint64_t)INT_MAX;
				}
				tsf_render_short(d->synth, buf + (size_t)done * 2, (int)before, 0);
				done += before;
				d->pos += before;
			}
			apply_event(d, &d->events[d->next]);
			d->next++;
			applied = true;
		}

		uint64_t rest = chunk_end - d->pos;
		if (rest > 0) {
			if (rest > (uint64_t)INT_MAX) {
				rest = (uint64_t)INT_MAX;
			}
			tsf_render_short(d->synth, buf + (size_t)done * 2, (int)rest, 0);
			done += rest;
			d->pos += rest;
		}
		if (!applied && rest == 0) {
			break; // no progress possible
		}
	}
	return done;
}

int mididec_seek(mididec_t *d, uint64_t frame) {
	if (!d) {
		return -1;
	}
	if (frame > d->total_frames) {
		frame = d->total_frames;
	}

	if (!d->synth) {
		// Metadata-only handle: nothing to replay, reads will return 0.
		d->pos = frame;
		d->next = d->count;
		return 0;
	}

	if (tsf_reset) {
		tsf_reset(d->synth);
	} else {
		tsf_close(d->synth);
		d->synth = d->soundfont ? tsf_load_filename(d->soundfont) : NULL;
		if (!d->synth) {
			return -1;
		}
		tsf_set_output(d->synth, TSF_STEREO_INTERLEAVED, MIDIDEC_RATE, 0.0f);
	}
	reset_channel_state(d);
	d->pos = 0;
	d->next = 0;

	while (d->next < d->count && d->events[d->next].frame <= frame) {
		apply_event(d, &d->events[d->next]);
		d->next++;
	}
	d->pos = frame;
	return 0;
}

int mididec_channels(const mididec_t *d) {
	return d ? 2 : 0;
}

int mididec_sample_rate(const mididec_t *d) {
	return d ? MIDIDEC_RATE : 0;
}

uint64_t mididec_total_frames(const mididec_t *d) {
	return d ? d->total_frames : 0;
}

const char *mididec_title(const mididec_t *d) {
	return (d && d->title[0]) ? d->title : "";
}
