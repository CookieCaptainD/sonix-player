// Host tests for the dim-before-screen-off rules. Compile this file on its
// own: dimlogic.h holds pure rules, with no LVGL or hardware dependency.
#include "src/system/device/dimlogic.h"

#include <stdio.h>

#define EXPECT(cond)                                                                       \
	do {                                                                                   \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
			return 1;                                                                      \
		}                                                                                  \
	} while (0)

static int timing_tests(void) {
	// A dim has to land while the screen is still on, with a visible lead
	// before the blank.
	EXPECT(dim_delay_fits(5000, 30000));
	EXPECT(dim_delay_fits(5000, 15000));
	EXPECT(dim_delay_fits(1000, 3000));

	// Equal to the timeout, past it, or too close to it: never seen.
	EXPECT(!dim_delay_fits(30000, 30000));
	EXPECT(!dim_delay_fits(31000, 30000));
	EXPECT(!dim_delay_fits(29500, 30000));

	// Zero delay is "dim now", zero timeout is "never blanks". Neither is a
	// dim before a screen-off.
	EXPECT(!dim_delay_fits(0, 30000));
	EXPECT(!dim_delay_fits(5000, 0));
	EXPECT(dim_effective_delay(5000, 0) == 0);

	// The effective delay is the requested one when it fits, and zero
	// otherwise, so the switch alone can never dim a screen that never blanks.
	EXPECT(dim_effective_delay(5000, 30000) == 5000);
	EXPECT(dim_effective_delay(29500, 30000) == 0);

	// The slider only offers the delays that fit the current timeout.
	const uint32_t options[] = {5000, 10000, 15000, 30000};
	EXPECT(dim_option_for_timeout(options, 4, 30000) == 2);
	EXPECT(dim_option_for_timeout(options, 4, 15000) == 1);
	EXPECT(dim_option_for_timeout(options, 4, 6000) == 0);
	EXPECT(dim_option_for_timeout(options, 4, 0) == -1);
	return 0;
}

int main(void) {
	if (timing_tests() != 0) {
		return 1;
	}
	puts("test_dim: ok");
	return 0;
}
