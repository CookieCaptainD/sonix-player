// Host checks for the DoP output decision. No sound card is involved: the
// ALSA probe is replaced by dsdroute_test.h.
#include "src/system/audio/dsdroute.h"
#include "src/system/audio/dsdroute_test.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                             \
	do {                                                                        \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                         \
		}                                                                       \
	} while (0)

int main(void) {
	char hw[64];

	dsdroute_hw_name("default", hw, sizeof(hw));
	CHECK(strcmp(hw, "hw:0") == 0);
	dsdroute_hw_name("plughw:1,0", hw, sizeof(hw));
	CHECK(strcmp(hw, "hw:1,0") == 0);
	dsdroute_hw_name("bluealsa", hw, sizeof(hw));
	CHECK(strcmp(hw, "bluealsa") == 0);

	CHECK(dsdroute_should_convert(true, false, false));
	CHECK(!dsdroute_should_convert(true, false, true));
	CHECK(dsdroute_should_convert(true, true, true));
	CHECK(!dsdroute_should_convert(false, false, false));

	CHECK(dsdroute_label_multiple(256, true) == 0);
	CHECK(dsdroute_label_multiple(256, false) == 256);
	CHECK(dsdroute_label_multiple(128, false) == 128);

	dsdroute_test_supported_rate = false;
	CHECK(!dsdroute_carries_dop("plughw:1,0", 705600, 32, 2));
	CHECK(strcmp(dsdroute_test_last_hw, "hw:1,0") == 0);
	dsdroute_test_supported_rate = true;
	CHECK(dsdroute_carries_dop("default", 352800, 32, 2));
	CHECK(strcmp(dsdroute_test_last_hw, "hw:0") == 0);
	CHECK(!dsdroute_carries_dop("default", 352800, 16, 2));

	CHECK(dsdroute_force_hw_volume(true, false));
	CHECK(dsdroute_force_hw_volume(false, true));
	CHECK(!dsdroute_force_hw_volume(false, false));

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("dsdroute: all checks passed\n");
	return 0;
}
