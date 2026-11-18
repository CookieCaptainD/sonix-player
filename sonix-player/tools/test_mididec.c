#define _XOPEN_SOURCE 700

// Host bench for mididec.c.
//
// No .sf2 is installed on this machine, so the parser and duration are tested
// with real SMF bytes while the TinySoundFont calls are answered by a small
// stub below. This checks event timing, rendering, seek/replay and the
// metadata-only path in a deterministic way; it does not validate sound quality
// or a real SoundFont load. Set MIDIDEC_TEST_SF2 to a real .sf2 to exercise
// tsf_load_filename as well.

#include "src/system/decode/mididec.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                  \
	do {                                                                         \
		if (!(cond)) {                                                           \
			printf("FAIL line %d: %s\n", __LINE__, #cond);                    \
			failures++;                                                      \
		}                                                                    \
	} while (0)

// ---------------------------------------------------------------------------
// TinySoundFont stub
// ---------------------------------------------------------------------------

struct tsf {
	int dummy;
};

typedef struct tsf tsf;

static int load_calls;
static int close_calls;
static int reset_calls;
static int note_on_calls;
static int note_off_calls;
static int program_calls;
static int render_calls;
static int last_key;
static int last_program;
static int last_mode;
static int last_rate;

tsf *tsf_load_filename(const char *path) {
	(void)path;
	load_calls++;
	struct tsf *f = (struct tsf *)malloc(sizeof(*f));
	if (f) {
		f->dummy = 1;
	}
	return f;
}

void tsf_close(tsf *f) {
	close_calls++;
	free(f);
}

#ifndef TEST_NO_TSF_RESET
void tsf_reset(tsf *f) {
	reset_calls++;
	f->dummy = 2;
}
#endif

void tsf_set_output(tsf *f, int mode, int rate, float gain) {
	(void)f;
	(void)gain;
	last_mode = mode;
	last_rate = rate;
}

int tsf_note_on(tsf *f, int program, int key, float velocity) {
	(void)f;
	(void)velocity;
	note_on_calls++;
	last_key = key;
	last_program = program;
	return 1;
}

void tsf_note_off(tsf *f, int program, int key) {
	(void)f;
	(void)program;
	note_off_calls++;
	last_key = key;
}

void tsf_program_change(tsf *f, int program, int bank) {
	(void)f;
	(void)bank;
	program_calls++;
	last_program = program;
}

void tsf_render_short(tsf *f, short *buffer, int samples, int mix) {
	(void)f;
	(void)mix;
	render_calls++;
	if (samples > 0 && buffer) {
		memset(buffer, 0, (size_t)samples * 2 * sizeof(short));
	}
}

static void reset_counters(void) {
	load_calls = close_calls = reset_calls = 0;
	note_on_calls = note_off_calls = program_calls = render_calls = 0;
}

// ---------------------------------------------------------------------------
// SMF builder
// ---------------------------------------------------------------------------

typedef struct {
	unsigned char b[1024];
	size_t n;
} blob;

static void put(blob *o, unsigned char c) {
	o->b[o->n++] = c;
}

static void put16(blob *o, unsigned int v) {
	put(o, (unsigned char)(v >> 8));
	put(o, (unsigned char)v);
}

static void put32(blob *o, unsigned int v) {
	put(o, (unsigned char)(v >> 24));
	put(o, (unsigned char)(v >> 16));
	put(o, (unsigned char)(v >> 8));
	put(o, (unsigned char)v);
}

static void put_varlen(blob *o, unsigned int v) {
	unsigned char tmp[4];
	int n = 0;
	tmp[n++] = (unsigned char)(v & 0x7F);
	v >>= 7;
	while (v) {
		tmp[n++] = (unsigned char)((v & 0x7F) | 0x80);
		v >>= 7;
	}
	while (n > 0) {
		put(o, tmp[--n]);
	}
}

static void put_text(blob *o, const char *s) {
	while (*s) {
		put(o, (unsigned char)*s++);
	}
}

static size_t track_begin(blob *o) {
	size_t start = o->n;
	put_text(o, "MTrk");
	put32(o, 0);
	return start;
}

static void track_end(blob *o, size_t start) {
	size_t len = o->n - start - 8;
	o->b[start + 4] = (unsigned char)(len >> 24);
	o->b[start + 5] = (unsigned char)(len >> 16);
	o->b[start + 6] = (unsigned char)(len >> 8);
	o->b[start + 7] = (unsigned char)len;
}

// Type 0: tempo 500000 us/qn to tick 480, then 400000 us/qn to tick 960.
// Last event tick 960 -> 0.5 + 0.4 = 0.9 s -> 39690 frames at 44100 Hz.
static void build_type0(blob *o) {
	put_text(o, "MThd");
	put32(o, 6);
	put16(o, 0); // format 0
	put16(o, 1); // one track
	put16(o, 480);

	size_t tr = track_begin(o);
	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x03);
	put(o, 9);
	put_text(o, "Test Tune");

	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x51);
	put(o, 3);
	put(o, 0x07);
	put(o, 0xA1);
	put(o, 0x20); // 500000 us

	put_varlen(o, 0);
	put(o, 0xC0);
	put(o, 5); // program 5

	put_varlen(o, 0);
	put(o, 0xB0);
	put(o, 7);
	put(o, 100); // volume
	put_varlen(o, 0);
	put(o, 0xB0);
	put(o, 10);
	put(o, 64); // pan

	put_varlen(o, 0);
	put(o, 0x90);
	put(o, 60);
	put(o, 100);
	put_varlen(o, 240);
	put(o, 60);
	put(o, 0); // running status, velocity 0 = note off
	put_varlen(o, 0);
	put(o, 64);
	put(o, 127); // running status note on
	put_varlen(o, 240);
	put(o, 0x80);
	put(o, 64);
	put(o, 0); // tick 480

	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x51);
	put(o, 3);
	put(o, 0x06);
	put(o, 0x1A);
	put(o, 0x80); // 400000 us at tick 480

	put_varlen(o, 480);
	put(o, 0xB0);
	put(o, 123);
	put(o, 0); // all notes off at tick 960
	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x2F);
	put(o, 0); // end of track at tick 960

	track_end(o, tr);
}

// Type 1: two tracks, merged; last tick 480 at 120 BPM -> 22050 frames.
static void build_type1(blob *o) {
	put_text(o, "MThd");
	put32(o, 6);
	put16(o, 1);
	put16(o, 2);
	put16(o, 480);

	size_t t0 = track_begin(o);
	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x03);
	put(o, 5);
	put_text(o, "Multi");
	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x51);
	put(o, 3);
	put(o, 0x07);
	put(o, 0xA1);
	put(o, 0x20);
	put_varlen(o, 480);
	put(o, 0xFF);
	put(o, 0x2F);
	put(o, 0);
	track_end(o, t0);

	size_t t1 = track_begin(o);
	put_varlen(o, 0);
	put(o, 0x90);
	put(o, 72);
	put(o, 90);
	put_varlen(o, 480);
	put(o, 0x80);
	put(o, 72);
	put(o, 0);
	put_varlen(o, 0);
	put(o, 0xFF);
	put(o, 0x2F);
	put(o, 0);
	track_end(o, t1);
}

static int temp_file(const unsigned char *data, size_t len, char *path, size_t cap) {
	snprintf(path, cap, "/tmp/mididec_test_XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) {
		return -1;
	}
	if (write(fd, data, len) != (ssize_t)len) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv) {
	blob midi0;
	memset(&midi0, 0, sizeof(midi0));
	build_type0(&midi0);

	char midi_path[64];
	if (temp_file(midi0.b, midi0.n, midi_path, sizeof(midi_path)) != 0) {
		printf("FAIL: could not create temporary MIDI\n");
		return 1;
	}

	printf("test: metadata-only open (no SoundFont)\n");
	reset_counters();
	mididec_t *info = mididec_open_info(midi_path);
	CHECK(info != NULL);
	if (info) {
		CHECK(strcmp(mididec_title(info), "Test Tune") == 0);
		CHECK(mididec_channels(info) == 2);
		CHECK(mididec_sample_rate(info) == 44100);
		printf("  type0 total_frames=%llu\n", (unsigned long long)mididec_total_frames(info));
		CHECK(mididec_total_frames(info) == 39690);
		short buf[512];
		CHECK(mididec_read_s16(info, 256, buf) == 0);
		CHECK(mididec_seek(info, 1000) == 0);
		CHECK(mididec_read_s16(info, 256, buf) == 0);
		mididec_close(info);
	}
	CHECK(load_calls == 0);

	printf("test: missing or empty SoundFont is a hard error\n");
	CHECK(mididec_open(midi_path, "") == NULL);
	CHECK(mididec_open(midi_path, "/tmp/no_such_soundfont_12345.sf2") == NULL);
	CHECK(mididec_open("", "/tmp/no_such_soundfont_12345.sf2") == NULL);

	// A real .sf2 can be supplied to exercise the actual loader; otherwise a
	// dummy file is enough for the stubbed playback checks below.
	const char *sf2 = (argc > 1) ? argv[1] : getenv("MIDIDEC_TEST_SF2");
	char sf2_path[64];
	if (!sf2) {
		const unsigned char dummy[] = "RIFFdummy soundfont";
		if (temp_file(dummy, sizeof(dummy) - 1, sf2_path, sizeof(sf2_path)) != 0) {
			printf("FAIL: could not create dummy SoundFont\n");
			unlink(midi_path);
			return 1;
		}
		sf2 = sf2_path;
		printf("note: no real SF2 found; rendering is checked with a tsf stub\n");
	} else {
		printf("note: using SoundFont %s\n", sf2);
	}

	printf("test: playback, event timing and seek\n");
	reset_counters();
	mididec_t *d = mididec_open(midi_path, sf2);
	CHECK(d != NULL);
	if (d) {
		CHECK(load_calls == 1);
		CHECK(last_mode == 0 && last_rate == 44100);
		CHECK(mididec_total_frames(d) == 39690);

		short buf[512];
		uint64_t n = mididec_read_s16(d, 256, buf);
		CHECK(n == 256);
		CHECK(note_on_calls == 1); // only the key-60 note at tick 0 so far
		CHECK(program_calls == 1);
		CHECK(last_key == 60);
		CHECK(last_program == 5);
		CHECK(render_calls > 0);

		CHECK(mididec_seek(d, 22050) == 0); // 0.5 s -> tick 480
#ifdef TEST_NO_TSF_RESET
		CHECK(load_calls == 2); // reset absent: synth rebuilt from the SF2
		CHECK(reset_calls == 0);
#else
		CHECK(reset_calls == 1);
#endif
		CHECK(note_on_calls == 3); // replayed both notes
		CHECK(note_off_calls == 2);
		CHECK(program_calls == 2);

		n = mididec_read_s16(d, 256, buf);
		CHECK(n == 256);
		CHECK(note_on_calls == 3); // no event until tick 960

		CHECK(mididec_seek(d, mididec_total_frames(d) - 100) == 0);
		CHECK(mididec_read_s16(d, 256, buf) == 100); // clipped to the file end

		CHECK(mididec_seek(d, 999999) == 0); // clamped to the end
		CHECK(mididec_read_s16(d, 256, buf) == 0);
		mididec_close(d);
#ifdef TEST_NO_TSF_RESET
		CHECK(close_calls >= 1); // seeks also close the old synth
#else
		CHECK(close_calls == 1);
#endif
	}

	printf("test: type 1, two merged tracks\n");
	blob midi1;
	memset(&midi1, 0, sizeof(midi1));
	build_type1(&midi1);
	char midi1_path[64];
	if (temp_file(midi1.b, midi1.n, midi1_path, sizeof(midi1_path)) == 0) {
		mididec_t *one = mididec_open_info(midi1_path);
		CHECK(one != NULL);
		if (one) {
			CHECK(strcmp(mididec_title(one), "Multi") == 0);
			CHECK(mididec_total_frames(one) == 22050);
			mididec_close(one);
		}
		unlink(midi1_path);
	}

	printf("test: invalid MIDI is rejected\n");
	const unsigned char junk[] = {'n', 'o', 't', ' ', 'm', 'i', 'd', 'i'};
	char junk_path[64];
	if (temp_file(junk, sizeof(junk), junk_path, sizeof(junk_path)) == 0) {
		CHECK(mididec_open_info(junk_path) == NULL);
		unlink(junk_path);
	}

	if (sf2 == sf2_path) {
		unlink(sf2_path);
	}
	unlink(midi_path);

	printf(failures ? "FAILED: %d check(s)\n" : "OK: all checks passed\n", failures);
	return failures ? 1 : 0;
}
