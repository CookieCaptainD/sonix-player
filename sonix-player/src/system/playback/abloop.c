#include "abloop.h"

// The smallest stretch worth looping. Anything shorter is a slip of the finger,
// not a passage somebody wants to hear again.
#define ABLOOP_MIN_GAP 1.0

void abloop_reset(abloop_state_t *s) {
	s->a_set = false;
	s->b_set = false;
	s->a_secs = 0.0;
	s->b_secs = 0.0;
}

void abloop_clear(abloop_state_t *s) {
	s->a_set = false;
	s->b_set = false;
}

void abloop_set_a(abloop_state_t *s, double secs) {
	s->a_set = true;
	s->a_secs = secs;
	if (s->b_set && secs >= s->b_secs - ABLOOP_MIN_GAP)
		s->b_set = false;
}

bool abloop_set_b(abloop_state_t *s, double secs) {
	if (!s->a_set || secs < s->a_secs + ABLOOP_MIN_GAP)
		return false;
	s->b_set = true;
	s->b_secs = secs;
	return true;
}

bool abloop_active(const abloop_state_t *s) { return s->enabled && s->a_set && s->b_set; }

bool abloop_should_loop(const abloop_state_t *s, double prev_secs, double cur_secs) { return abloop_active(s) && prev_secs < s->b_secs && cur_secs >= s->b_secs; }
