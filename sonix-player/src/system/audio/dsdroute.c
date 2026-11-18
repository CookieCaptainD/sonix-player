#include "dsdroute.h"
#include "dsdroute_test.h"

#include <stdio.h>
#include <string.h>

#ifndef DSDROUTE_TEST
#include <alsa/asoundlib.h>
#endif

#ifdef DSDROUTE_TEST
bool dsdroute_test_supported_rate = true;
char dsdroute_test_last_hw[64];
#endif

void dsdroute_hw_name(const char *output, char *out, size_t out_size) {
	if (!out_size) {
		return;
	}
	if (!output || !*output) {
		out[0] = '\0';
		return;
	}
	if (strcmp(output, "default") == 0) {
		snprintf(out, out_size, "hw:0");
	} else if (strncmp(output, "plughw:", 7) == 0) {
		snprintf(out, out_size, "hw:%s", output + 7);
	} else {
		snprintf(out, out_size, "%s", output);
	}
}

bool dsdroute_carries_dop(const char *output, int carrier_rate, int bits, int channels) {
	char hw[64];
	dsdroute_hw_name(output, hw, sizeof(hw));

	if (carrier_rate <= 0 || channels < 1 || bits != 32) {
		return false;
	}

#ifdef DSDROUTE_TEST
	snprintf(dsdroute_test_last_hw, sizeof(dsdroute_test_last_hw), "%s", hw);
	return dsdroute_test_supported_rate;
#else
	snd_pcm_t *pcm = NULL;
	if (snd_pcm_open(&pcm, hw, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		return false;
	}

	snd_pcm_hw_params_t *params = NULL;
	snd_pcm_hw_params_alloca(&params);
	bool ok = snd_pcm_hw_params_any(pcm, params) >= 0 &&
			  snd_pcm_hw_params_test_rate(pcm, params, (unsigned int)carrier_rate, 0) >= 0 &&
			  snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_S32_LE) >= 0 &&
			  snd_pcm_hw_params_test_channels(pcm, params, (unsigned int)channels) >= 0;

	snd_pcm_close(pcm);
	return ok;
#endif
}

bool dsdroute_should_convert(bool passthrough, bool bluetooth, bool output_carries_dop) {
	return passthrough && (bluetooth || !output_carries_dop);
}

int dsdroute_label_multiple(int decoder_multiple, bool converted) {
	return converted ? 0 : decoder_multiple;
}

bool dsdroute_force_hw_volume(bool dsd_active, bool configured) {
	return dsd_active || configured;
}
