// Host checks for the volume step added to the internet radio write path.
// The swvolume functions are stubbed so the dispatch and the sample count can
// be checked without the player's audio stack.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/streaming/radio_volume.h"

static int active;
static int calls_s16;
static int calls_s32;
static void *seen_ptr;
static int seen_count;

bool swvolume_active(void)
{
	return active != 0;
}

void swvolume_apply_s16(short *samples, int count)
{
	calls_s16++;
	seen_ptr = samples;
	seen_count = count;
}

void swvolume_apply_s32(int32_t *samples, int count)
{
	calls_s32++;
	seen_ptr = samples;
	seen_count = count;
}

static int failures;

#define CHECK(cond)                                                                  \
	do {                                                                             \
		if (!(cond)) {                                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			failures++;                                                              \
		}                                                                            \
	} while (0)

static void reset(void)
{
	active = 0;
	calls_s16 = 0;
	calls_s32 = 0;
	seen_ptr = NULL;
	seen_count = 0;
}

static void check_wiring(void)
{
	FILE *f = fopen("src/system/streaming/radio.c", "r");
	char *src;
	long size;
	const char *apply = "radio_volume_apply(";
	const char *write = "audio_external_write(";
	int applies[4];
	int n_apply = 0;
	const char *p;

	CHECK(f != NULL);
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	src = malloc((size_t)size + 1);
	CHECK(src != NULL);
	if (!src) {
		fclose(f);
		return;
	}
	CHECK(fread(src, 1, (size_t)size, f) == (size_t)size);
	src[size] = '\0';
	fclose(f);

	p = src;
	while ((p = strstr(p, apply)) != NULL && n_apply < 4) {
		applies[n_apply++] = (int)(p - src);
		p += strlen(apply);
	}
	CHECK(n_apply == 4);
	if (n_apply != 4) {
		free(src);
		return;
	}
	for (int i = 0; i < 4; i++) {
		const char *at = src + applies[i];
		const char *next_write = strstr(at, write);
		const char *next_apply = strstr(at + strlen(apply), apply);
		CHECK(next_write != NULL);
		if (!next_write)
			continue;
		CHECK(next_apply == NULL || next_write < next_apply);
	}
	free(src);
}

int main(void)
{
	short pcm16[8];
	int32_t pcm32[8];
	unsigned char pcm24[12];

	reset();
	radio_volume_apply(pcm16, 2, 16);
	CHECK(calls_s16 == 0);
	CHECK(calls_s32 == 0);

	reset();
	active = 1;
	radio_volume_apply(pcm16, 3, 16);
	CHECK(calls_s16 == 1);
	CHECK(calls_s32 == 0);
	CHECK(seen_ptr == pcm16);
	CHECK(seen_count == 6);

	reset();
	active = 1;
	radio_volume_apply(pcm32, 3, 32);
	CHECK(calls_s32 == 1);
	CHECK(calls_s16 == 0);
	CHECK(seen_ptr == pcm32);
	CHECK(seen_count == 6);

	reset();
	active = 1;
	radio_volume_apply(pcm24, 2, 24);
	CHECK(calls_s16 == 0);
	CHECK(calls_s32 == 0);

	check_wiring();

	if (failures) {
		fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	printf("radio volume: all checks passed\n");
	return 0;
}
