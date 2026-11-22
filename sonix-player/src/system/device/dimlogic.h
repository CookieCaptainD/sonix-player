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

// Whether this tick should start a dim. Idle is the panel's own inactivity
// clock (touch and buttons), so the same measure that blanks the screen drives
// the dim. A screen that is held on for a job, or already off, is left alone.
static inline bool dim_should_apply(bool enabled, bool screen_on, bool screen_held, uint32_t effective_delay_ms,
									uint32_t idle_ms) {
	return enabled && screen_on && !screen_held && effective_delay_ms > 0 && idle_ms >= effective_delay_ms;
}

// Whether interaction has ended a dim. Activity drops the same idle clock
// back below the delay, which is how a touch or a key restores the panel
// without a separate notification path.
static inline bool dim_should_cancel(bool active, uint32_t effective_delay_ms, uint32_t idle_ms) {
	return active && effective_delay_ms > 0 && idle_ms < effective_delay_ms;
}

// One dim cycle: the level that was on the panel before the dim, so it can be
// put back when the user returns.
typedef struct {
	bool active;
	long restore_brightness;
} dim_state_t;

static inline void dim_state_reset(dim_state_t *s) {
	s->active = false;
	s->restore_brightness = 0;
}

// Returns the level to write, or -1 when already dimmed. The on-level is kept
// as-is: only the backlight output changes, never the configured brightness.
static inline long dim_state_begin(dim_state_t *s, long on_brightness, long dim_level) {
	if (s->active) {
		return -1;
	}
	s->active = true;
	s->restore_brightness = on_brightness;
	return dim_level;
}

// Returns the level to restore, or -1 when nothing was dimmed. The state is
// cleared either way, so a stale level cannot be written on a later wake.
static inline long dim_state_cancel(dim_state_t *s) {
	if (!s->active) {
		return -1;
	}
	long restore = s->restore_brightness;
	dim_state_reset(s);
	return restore;
}

#endif // DIMLOGIC_H
