#ifndef DIMLOGIC_H
#define DIMLOGIC_H

#include <stdbool.h>
#include <stdint.h>

// Pure rules for dimming before the screen blanks. No LVGL or hardware
// dependency, so the host test can compile this header on its own.

// Leave this much time between the dim and the blank. A dim that lands at the
// same moment as the blank is never seen, and too short a lead reads as a
// glitch rather than as a warning.
#define DIM_LEAD_MIN_MS 1000u

// True when a dim at `delay_ms` happens while the screen is still on and
// before it blanks at `timeout_ms`. A zero timeout means the screen never
// blanks on its own, so there is nothing to precede.
static inline bool dim_delay_fits(uint32_t delay_ms, uint32_t timeout_ms) {
	if (delay_ms == 0 || timeout_ms == 0 || delay_ms >= timeout_ms) {
		return false;
	}
	return (timeout_ms - delay_ms) >= DIM_LEAD_MIN_MS;
}

// The delay handed to the power manager. Zero means no dimming for this
// timeout, whatever the switch says: with the screen set to stay on, or with a
// delay that no longer fits, the switch cannot dim anything.
static inline uint32_t dim_effective_delay(uint32_t delay_ms, uint32_t timeout_ms) {
	return dim_delay_fits(delay_ms, timeout_ms) ? delay_ms : 0;
}

// Index of the largest of `count` ascending delay options that still fits
// before `timeout_ms`, or -1 when none does. The slider uses this to narrow
// its range when the screen-off time changes.
static inline int dim_option_for_timeout(const uint32_t *options, int count, uint32_t timeout_ms) {
	int best = -1;
	for (int i = 0; i < count; i++) {
		if (dim_delay_fits(options[i], timeout_ms)) {
			best = i;
		}
	}
	return best;
}

#endif // DIMLOGIC_H
