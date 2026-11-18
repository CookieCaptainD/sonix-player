#ifndef ABLOOP_H
#define ABLOOP_H

#include <stdbool.h>

// A pair of points in the current track. Kept on its own, with no LVGL or audio
// dependencies, so the UI can drive the state and the player can feed it from a
// tick without either having to know how the other works.
//
// enabled is the switch: points alone are not a loop until it is on. show_button
// is only a hint for the Now Playing page; none of the functions here touch it.
typedef struct {
	bool enabled;
	bool show_button;
	bool a_set;
	bool b_set;
	double a_secs;
	double b_secs;
} abloop_state_t;

// Drop both points. Leaves enabled and show_button as they were.
void abloop_reset(abloop_state_t *s);

// Drop only the points, leaving the switch and the button alone.
void abloop_clear(abloop_state_t *s);

// Place A at secs. If B is set and would land less than a second later, B goes.
void abloop_set_a(abloop_state_t *s, double secs);

// Place B at secs. Refused when A is unset or B would be less than a second past
// A: the state is left untouched and false is returned. Otherwise true.
bool abloop_set_b(abloop_state_t *s, double secs);

// True only with the switch on and both points placed.
bool abloop_active(const abloop_state_t *s);

// True once, when a playing tick crosses B from below: prev was short of B and
// cur has reached it. No loop with the switch off, a point missing, or a tick
// that starts already past B.
bool abloop_should_loop(const abloop_state_t *s, double prev_secs, double cur_secs);

#endif /* ABLOOP_H */
