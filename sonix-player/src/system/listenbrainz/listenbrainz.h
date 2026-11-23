#ifndef LISTENBRAINZ_H
#define LISTENBRAINZ_H

#include <stdbool.h>

// Scrobbling to ListenBrainz.
//
// Mirrors the Last.fm integration: the GUI thread reports playback once a
// second with listenbrainz_note_playback(), and this module decides when a
// play counts (half its length or four minutes, whichever comes first, for a
// track of at least 30 seconds).
//
// A worker thread does the network and the files. "Now playing" and scrobbles
// are submitted to the ListenBrainz submit-listens API when there is Wi-Fi.
// Scrobbles are queued in .local on the card and sent in batches whenever the
// Wi-Fi is up. Nothing here turns the Wi-Fi on.
//
// Authentication uses a ListenBrainz user token (from listenbrainz.org
// settings), kept beside device_config.ini; it is never shown back to the UI
// once stored, only the associated user name is.

typedef enum {
	LISTENBRAINZ_STATE_UNAVAILABLE,
	LISTENBRAINZ_STATE_OFF,
	LISTENBRAINZ_STATE_SIGNED_OUT,
	LISTENBRAINZ_STATE_SIGNING_IN,
	LISTENBRAINZ_STATE_CONNECTED,
	LISTENBRAINZ_STATE_SESSION_EXPIRED, // token refused: add a valid token again
	LISTENBRAINZ_STATE_REFUSED,			// request refused; `detail` says why
} listenbrainz_state_t;

typedef enum {
	LISTENBRAINZ_LOGIN_OK,
	LISTENBRAINZ_LOGIN_WRONG_CREDENTIALS,
	LISTENBRAINZ_LOGIN_NO_REPLY,
	LISTENBRAINZ_LOGIN_REFUSED, // `detail` says why
} listenbrainz_login_result_t;

typedef struct {
	listenbrainz_state_t state;
	bool enabled;
	char user[128];
	char detail[160];
	int queued;

	unsigned login_serial;
	listenbrainz_login_result_t login_result;
} listenbrainz_status_t;

void listenbrainz_init(void);

bool listenbrainz_available(void);

void listenbrainz_set_enabled(bool on);

bool listenbrainz_active(void);

// Validates `token` (needs Wi-Fi), stores it and the user name on success.
// Returns at once; the worker does the work. The token is wiped once used.
void listenbrainz_login(const char *token);
void listenbrainz_logout(void);

void listenbrainz_get_status(listenbrainz_status_t *out);

typedef struct {
	bool playing;
	const char *file;
	double position;
	double duration;
	const char *artist;
	const char *title;
	const char *album;
	bool skip;
} listenbrainz_playback_t;

// GUI thread, about once a second.
void listenbrainz_note_playback(const listenbrainz_playback_t *now);

// True while a request is on the wire.
bool listenbrainz_network_wanted(void);

#endif /* LISTENBRAINZ_H */
