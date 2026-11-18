// Host checks for the pure A/B loop state module. No LVGL or audio stack here:
// just the state transitions the Now Playing page will drive.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "src/system/playback/abloop.h"

int main(void) {
	abloop_state_t s = {0};

	// A fresh state has no loop, and nothing to loop even if time jumps.
	assert(!abloop_active(&s));
	assert(!abloop_should_loop(&s, 9.0, 10.0));

	// Points alone are not a loop: the switch has to be on as well.
	abloop_set_a(&s, 10.0);
	abloop_set_b(&s, 30.0);
	assert(s.a_set && s.b_set);
	assert(!abloop_active(&s));
	s.enabled = true;
	assert(abloop_active(&s));

	// Crossing B from below fires once; going back to A or starting short does not.
	assert(abloop_should_loop(&s, 29.9, 30.1));
	assert(!abloop_should_loop(&s, 30.1, 10.0));
	assert(!abloop_should_loop(&s, 5.0, 9.9));

	// B must stay at least a second past A. A too-close B is refused, old B kept.
	assert(!abloop_set_b(&s, 10.5));
	assert(s.b_set && s.b_secs == 30.0);
	assert(abloop_set_b(&s, 11.0));

	// Moving A up to or past B drops the old point, rather than leaving them crossed.
	abloop_set_a(&s, 25.0);
	assert(s.a_set && !s.b_set);

	// Clearing drops both points but leaves the switch and the button alone.
	s.show_button = true;
	abloop_clear(&s);
	assert(!s.a_set && !s.b_set);
	assert(s.enabled && s.show_button);

	// Same for a reset: points go, switch and button stay.
	abloop_set_a(&s, 1.0);
	abloop_set_b(&s, 5.0);
	abloop_reset(&s);
	assert(!s.a_set && !s.b_set);
	assert(s.enabled && s.show_button);

	puts("abloop: ok");
	return 0;
}
