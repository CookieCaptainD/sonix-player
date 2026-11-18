// Host-only bench for trackerdec (libxmp). Builds the smallest useful MOD:
// one 64-row pattern, four channels, M.K. signature, one sample with a square
// wave so the decoded PCM is not silent.
//
// Uses only stdio/stdlib/string and the trackerdec API itself.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/decode/trackerdec.h"

#define TEST_PATH "/tmp/trackerdec_test.mod"
#define TEST_TITLE "Tracker Test"
#define TEST_FRAMES 1024
#define GUARD 0x5a5a

static int failures;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
			failures++;                                                      \
		}                                                                        \
	} while (0)

static void put16be(unsigned char *p, unsigned v) {
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void write_test_mod(const char *path) {
	unsigned char mod[20 + 31 * 30 + 1 + 1 + 128 + 4 + 64 * 4 * 4 + 128];
	memset(mod, 0, sizeof(mod));

	// Title (20 bytes).
	memcpy(mod, TEST_TITLE, strlen(TEST_TITLE));

	// Sample 1 header: 64 words of 16-bit data, volume 64, no loop.
	unsigned char *ins = mod + 20;
	memcpy(ins, "Square", 6);
	put16be(ins + 22, 64);  // length in 16-bit words
	ins[24] = 0;            // finetune
	ins[25] = 64;           // volume
	put16be(ins + 26, 0);   // loop start
	put16be(ins + 28, 0);   // loop size

	mod[20 + 31 * 30] = 1;         // one order
	mod[20 + 31 * 30 + 1] = 0x7f;  // standard Protracker restart byte
	                               // order table stays zero

	unsigned char *magic = mod + 20 + 31 * 30 + 2 + 128;
	memcpy(magic, "M.K.", 4);

	// Pattern: a note in channel 0 on every row, period 428 (in MOD range),
	// no effect. Period 428 = 0x01ac, sample number 1.
	unsigned char *pat = magic + 4;
	for (int row = 0; row < 64; row++) {
		unsigned char *note = pat + (size_t)row * 4 * 4;
		note[0] = 0x01;
		note[1] = 0xac;
		note[2] = 0x10;
		note[3] = 0x00;
	}

	// Square wave in the sample data: 64 words = 128 bytes.
	short *samples = (short *)(void *)(pat + 64 * 4 * 4);
	for (int i = 0; i < 64; i++) {
		samples[i] = (i & 16) ? 12000 : -12000;
	}

	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f) {
		return;
	}
	CHECK(fwrite(mod, 1, sizeof(mod), f) == sizeof(mod));
	fclose(f);
}

int main(void) {
	// Bad input must fail cleanly, including before anything is allocated.
	CHECK(trackerdec_open("/tmp/trackerdec_no_such_file.mod") == NULL);
	CHECK(trackerdec_open(NULL) == NULL);
	CHECK(trackerdec_channels(NULL) == 0);
	CHECK(trackerdec_sample_rate(NULL) == 0);
	CHECK(trackerdec_total_frames(NULL) == 0);
	CHECK(trackerdec_title(NULL)[0] == '\0');

	write_test_mod(TEST_PATH);

	trackerdec_t *d = trackerdec_open(TEST_PATH);
	CHECK(d != NULL);
	if (!d) {
		remove(TEST_PATH);
		return 1;
	}

	CHECK(trackerdec_channels(d) == 2);
	CHECK(trackerdec_sample_rate(d) == 44100);
	CHECK(strcmp(trackerdec_title(d), TEST_TITLE) == 0);

	uint64_t total = trackerdec_total_frames(d);
	CHECK(total > 0);
	printf("title='%s' channels=%d rate=%d total=%llu\n", trackerdec_title(d),
	       trackerdec_channels(d), trackerdec_sample_rate(d),
	       (unsigned long long)total);

	// Guard region catches overruns past frames * 2 shorts.
	short *buf = malloc((TEST_FRAMES * 2 + GUARD) * sizeof(short));
	CHECK(buf != NULL);
	if (!buf) {
		trackerdec_close(d);
		remove(TEST_PATH);
		return 1;
	}
	for (int i = 0; i < GUARD; i++) {
		buf[TEST_FRAMES * 2 + i] = GUARD;
	}

	uint64_t got = trackerdec_read_s16(d, TEST_FRAMES, buf);
	CHECK(got > 0 && got <= TEST_FRAMES);
	int nonzero = 0;
	for (uint64_t i = 0; i < got * 2; i++) {
		if (buf[i] != 0) {
			nonzero++;
		}
	}
	CHECK(nonzero > 0);
	for (int i = 0; i < GUARD; i++) {
		CHECK(buf[TEST_FRAMES * 2 + i] == GUARD);
	}
	printf("read=%llu nonzero=%d\n", (unsigned long long)got, nonzero);

	// Seek to the middle: decoding must resume, not start from zero.
	uint64_t mid = total / 2;
	CHECK(trackerdec_seek(d, mid) != 0);
	got = trackerdec_read_s16(d, TEST_FRAMES, buf);
	CHECK(got > 0 && got <= TEST_FRAMES);
	printf("after seek to %llu: read=%llu\n", (unsigned long long)mid,
	       (unsigned long long)got);

	// Seeking past the end clamps; the next read reports EOF.
	CHECK(trackerdec_seek(d, total + 100000) != 0);
	got = trackerdec_read_s16(d, TEST_FRAMES, buf);
	CHECK(got == 0);
	CHECK(trackerdec_read_s16(d, TEST_FRAMES, buf) == 0);
	printf("at end: read=%llu\n", (unsigned long long)got);

	// A fresh decoder, drained to completion, must stop on its own (no loop).
	trackerdec_t *d2 = trackerdec_open(TEST_PATH);
	CHECK(d2 != NULL);
	if (d2) {
		uint64_t seen = 0;
		while ((got = trackerdec_read_s16(d2, 8192, buf)) > 0) {
			seen += got;
			CHECK(seen <= trackerdec_total_frames(d2));
		}
		CHECK(seen > 0);
		printf("drained=%llu of total=%llu\n", (unsigned long long)seen,
		       (unsigned long long)trackerdec_total_frames(d2));
		trackerdec_close(d2);
	}

	free(buf);
	trackerdec_close(d);
	remove(TEST_PATH);

	if (failures) {
		printf("FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("ALL OK\n");
	return 0;
}
