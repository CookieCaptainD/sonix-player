#ifndef SONIX_USBTRANSPORT_H
#define SONIX_USBTRANSPORT_H

#include <stdbool.h>

// A USB audio output that the listener has set to automatic goes out at unity
// gain, index 100. The caller applies the returned level through the normal
// volume path; -1 means keep the level already in hand.
int usbtransport_forced_percent(bool usb_output, bool bluetooth_active, bool automatic, int remembered);

// [usb] transport_auto_volume, on by default.
bool usbtransport_automatic_enabled(void);

#endif
