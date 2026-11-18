#ifndef DSDROUTE_H
#define DSDROUTE_H

#include <stdbool.h>
#include <stddef.h>

// DoP carries DSD in 24-bit words inside a 32-bit stream. ALSA's plug layer
// accepts any rate and resamples behind the caller's back, which destroys the
// marker byte. Every DoP decision must use the rate the hardware can carry.

// Turn the playback device name into the hardware device behind it:
// "default" -> "hw:0", "plughw:1,0" -> "hw:1,0". Other names pass through.
void dsdroute_hw_name(const char *output, char *out, size_t out_size);

// True when the hardware device accepts this DoP carrier at this format.
bool dsdroute_carries_dop(const char *output, int carrier_rate, int bits, int channels);

// The single gate used before the decoder switches DSD to PCM.
bool dsdroute_should_convert(bool passthrough, bool bluetooth, bool output_carries_dop);

// The format label follows the stream that leaves the player, not the file.
int dsdroute_label_multiple(int decoder_multiple, bool converted);

// A DoP stream cannot be attenuated in software, so its volume must sit on
// the device control even when the user has not opted into hardware volume.
bool dsdroute_force_hw_volume(bool dsd_active, bool configured);

#endif /* DSDROUTE_H */
