#include "usbtransport.h"

#include "src/system/core/config.h"

int usbtransport_forced_percent(bool usb_output, bool bluetooth_active, bool automatic, int remembered) {
	(void)remembered;
	if (!usb_output || bluetooth_active || !automatic) {
		return -1;
	}
	return 100;
}

bool usbtransport_automatic_enabled(void) { return config_get_bool("usb", "transport_auto_volume", true); }
