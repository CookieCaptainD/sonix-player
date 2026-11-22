#ifndef BROWSERREMOTE_HTTP_H
#define BROWSERREMOTE_HTTP_H

#include <stdbool.h>

// The browser remote's own HTTP service. It is deliberately separate from
// Wi-Fi transfer: no thttpd, no CGI, no shared document root. A small server
// runs inside the player on its own port while the remote page is open, and
// answers only the control page and a handful of JSON endpoints.

#define BROWSERREMOTE_PORT 4400

// How long a page may go without a request before the server gives the port
// back. The page polls, so a closed browser tab stops the server on its own.
#define BROWSERREMOTE_IDLE_SECONDS 20

// Loads the PIN and the saved preference from device_config.ini and installs
// the player operations. Does not start the server: it may only ever run while
// its page is open, which is not true at boot.
void browserremote_http_init(void);

// Starts or stops the listener. Both are safe to call repeatedly.
bool browserremote_http_start(void);
void browserremote_http_stop(void);
bool browserremote_http_running(void);

// Expires old sessions/challenges and stops an idle server. Called from the
// page's poll timer, the same place Wi-Fi transfer runs its watchdog.
void browserremote_http_watchdog(void);

// "http://<address>:4400", or an empty string when there is no Wi-Fi address.
void browserremote_http_url(char *out, int size);

// PIN set from the player UI. Stored as a hash in device_config.ini.
bool browserremote_http_has_pin(void);
void browserremote_http_set_pin(const char *pin);

// The saved on/off preference, used to show the switch on the page.
bool browserremote_http_get_enabled(void);
void browserremote_http_set_enabled(bool on);

#endif /* BROWSERREMOTE_HTTP_H */
