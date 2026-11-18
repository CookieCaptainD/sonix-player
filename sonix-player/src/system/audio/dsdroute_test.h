#ifndef DSDROUTE_TEST_H
#define DSDROUTE_TEST_H

// Test seam. Compiling dsdroute.c with -DDSDROUTE_TEST=1 replaces the ALSA
// probe with these two variables, so the decision and the device mapping can
// be checked on a host with no sound card.
#ifdef DSDROUTE_TEST

#include <stdbool.h>

extern bool dsdroute_test_supported_rate;
extern char dsdroute_test_last_hw[64];

#endif /* DSDROUTE_TEST */

#endif /* DSDROUTE_TEST_H */
