/*
 * Tests for the vendored DST decoder (src/system/decode/dst_decoder.c).
 *
 * Only the paths that need no arithmetic-coded data are exercised here: an
 * uncompressed frame and the argument checks. The compressed path is covered
 * by decoding a real SACD ISO in the reader's tests.
 */

#include <stdio.h>
#include <string.h>

#include "system/decode/dst_decoder.h"

#define CHANNELS 2
#define SAMPLES_PER_FRAME 4704 /* 588 * 2822400 / 44100 / 8 */
#define FRAME_BYTES (SAMPLES_PER_FRAME * CHANNELS)

static int failures;
static unsigned char output[FRAME_BYTES];

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

static void check(int cond, const char *what)
{
	if (!cond)
		fail(what);
}

/* An uncompressed DST frame: a zero bit, then five bits of zero (same
 * segmentation, same map, ...), then the raw DSD bytes. 9409 bytes total. */
static int test_uncompressed(void)
{
	unsigned char frame[1 + FRAME_BYTES];
	dst_decoder_t *dec = NULL;
	int out_len = -1;
	int ret, i;

	frame[0] = 0x00;
	for (i = 0; i < FRAME_BYTES; i++)
		frame[1 + i] = (unsigned char)(i * 37 + 11);

	memset(output, 0, sizeof(output));

	ret = dst_decoder_init(&dec, CHANNELS, 2822400);
	check(ret == 0 && dec != NULL, "init for the uncompressed frame");
	if (ret != 0 || !dec)
		return 1;

	ret = dst_decoder_decode(dec, frame, (int)sizeof(frame), output, &out_len);
	check(ret == 0, "uncompressed decode returns 0");
	check(out_len == FRAME_BYTES, "uncompressed decode produces 9408 bytes");
	if (ret == 0 && out_len == FRAME_BYTES)
		check(memcmp(output, frame + 1, FRAME_BYTES) == 0,
		      "uncompressed decode copies the frame through");

	dst_decoder_close(dec);
	return 0;
}

static void test_bad_input(void)
{
	unsigned char one = 0x00;
	dst_decoder_t *dec = NULL;
	int out_len = -1;
	int ret;

	if (dst_decoder_init(&dec, CHANNELS, 2822400) != 0 || !dec) {
		fail("init for the bad-input test");
		return;
	}

	out_len = -1;
	ret = dst_decoder_decode(dec, &one, 0, output, &out_len);
	check(ret < 0, "empty frame is rejected");

	out_len = -1;
	ret = dst_decoder_decode(dec, &one, 1, output, &out_len);
	check(ret < 0, "one-byte frame is rejected");

	dst_decoder_close(dec);
}

static void test_init_close(void)
{
	dst_decoder_t *dec = NULL;

	check(dst_decoder_init(&dec, CHANNELS, 2822400) == 0 && dec != NULL,
	      "init with 2 channels at 2822400");
	check(dst_decoder_close(dec) == 0, "close returns 0");
	check(dst_decoder_close(NULL) == 0, "close(NULL) returns 0");
}

int main(void)
{
	test_uncompressed();
	test_bad_input();
	test_init_close();

	if (failures) {
		printf("dst_decoder_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("dst_decoder_test: ok\n");
	return 0;
}
