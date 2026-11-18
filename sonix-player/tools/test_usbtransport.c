// Host checks for the automatic USB DAC transport level decision.
// The config read is stubbed so the pure decision can be tested without INI.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "src/system/audio/usbtransport.h"

static bool config_has_key;
static int config_value;
static char seen_section[32];
static char seen_key[32];
static bool seen_fallback;

bool config_get_bool(const char *section, const char *key, bool fallback) {
	snprintf(seen_section, sizeof(seen_section), "%s", section ? section : "");
	snprintf(seen_key, sizeof(seen_key), "%s", key ? key : "");
	seen_fallback = fallback;
	if (!config_has_key) {
		return fallback;
	}
	return config_value != 0;
}

static int failures;

static void expect_int(const char *name, int got, int want) {
	if (got == want) {
		return;
	}
	fprintf(stderr, "FAIL %s: got %d, want %d\n", name, got, want);
	failures++;
}

static void expect_bool(const char *name, bool got, bool want) {
	expect_int(name, got ? 1 : 0, want ? 1 : 0);
}

typedef struct {
	const char *name;
	bool usb_output;
	bool bluetooth_active;
	bool automatic;
	int remembered;
	int want;
} case_t;

int main(void) {
	static const case_t cases[] = {
		{"detect path, new DAC", true, false, true, 30, 100},
		{"USB DAC, automatic, remembered 42", true, false, true, 42, 100},
		{"USB DAC, automatic, remembered 0", true, false, true, 0, 100},
		{"USB DAC, automatic, remembered 100", true, false, true, 100, 100},
		{"USB DAC, automatic, Bluetooth active", true, true, true, 42, -1},
		{"USB DAC, automatic off", true, false, false, 42, -1},
		{"not USB output", false, false, true, 42, -1},
		{"Bluetooth output with automatic on", false, false, true, 42, -1},
		{"second call stays at 100", true, false, true, 42, 100},
		{"USB DAC, automatic, remembered 99", true, false, true, 99, 100},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int got = usbtransport_forced_percent(cases[i].usb_output, cases[i].bluetooth_active, cases[i].automatic,
											  cases[i].remembered);
		expect_int(cases[i].name, got, cases[i].want);
	}

	config_has_key = false;
	expect_bool("default when the key is absent", usbtransport_automatic_enabled(), true);
	expect_int("config section", strcmp(seen_section, "usb") == 0, 1);
	expect_int("config key", strcmp(seen_key, "transport_auto_volume") == 0, 1);
	expect_bool("default passed to config", seen_fallback, true);

	config_has_key = true;
	config_value = 0;
	expect_bool("switch off", usbtransport_automatic_enabled(), false);
	config_value = 1;
	expect_bool("switch on", usbtransport_automatic_enabled(), true);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("usbtransport: all checks passed\n");
	return 0;
}
