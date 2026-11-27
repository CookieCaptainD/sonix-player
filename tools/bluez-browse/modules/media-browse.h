/*
 * Asynchronous MPRIS browsing bridge used by the AVRCP target.
 *
 * Every call is sent with libdbus, given a 3000 ms deadline and retried once
 * on timeout.  Replies are accepted only when their D-Bus signature matches
 * what the caller expects.  MPRIS track ids are mapped to stable 64 bit UIDs
 * through a 512 entry LRU cache, because AVRCP UIDs must survive folder
 * changes while the D-Bus object paths may not.
 */

#ifndef BLUEZ_MEDIA_BROWSE_H
#define BLUEZ_MEDIA_BROWSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dbus/dbus.h>
#include <glib.h>

#define MEDIA_BROWSE_TIMEOUT_MS 3000
#define MEDIA_BROWSE_MAX_RETRIES 1
#define MEDIA_BROWSE_UID_CACHE 512

#define MEDIA_BROWSE_MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define MEDIA_BROWSE_MPRIS_TRACKLIST "org.mpris.MediaPlayer2.TrackList"
#define MEDIA_BROWSE_PROPERTIES "org.freedesktop.DBus.Properties"

struct media_browse_item {
	uint64_t uid;
	char *name;
	char *uri;
	char *mime;
	uint8_t type;
	bool playable;
};

struct media_browse_cb {
	void (*items)(GSList *items, uint32_t total, void *user_data);
	void (*error)(unsigned id, const char *dest, const char *message,
							void *user_data);
	void (*done)(unsigned id, bool success, void *user_data);
};

struct media_browse;

struct media_browse *media_browse_new(DBusConnection *conn,
				const struct media_browse_cb *cb, void *user_data);
void media_browse_free(struct media_browse *mb);

/* All of these return a non-zero call id, or 0 on immediate failure. */
unsigned media_browse_list_items(struct media_browse *mb, const char *dest,
					const char *path, uint32_t start,
					uint32_t end);
unsigned media_browse_get_all(struct media_browse *mb, const char *dest,
					const char *path);
unsigned media_browse_get_count(struct media_browse *mb, const char *dest,
					const char *path);
unsigned media_browse_change_folder(struct media_browse *mb, const char *dest,
					const char *path, uint64_t uid);
unsigned media_browse_play(struct media_browse *mb, const char *dest,
					const char *uri);

/* Cancel every outstanding call for a destination (e.g. player disconnect). */
void media_browse_cancel_dest(struct media_browse *mb, const char *dest);

/* Pump the bus and expire/retry pending calls.  timeout_ms <= 0 is nonblocking. */
void media_browse_dispatch(struct media_browse *mb, int timeout_ms);

/* Number of items reported by the last successful list/count call. */
uint32_t media_browse_count(struct media_browse *mb);
uint32_t media_browse_count_sync(struct media_browse *mb, const char *dest, const char *path);

/* Path helpers ("/Filesystem/A/B" -> "/Filesystem/A"). */
const char *media_browse_parent(const char *path, char *buf, size_t len);

/* UID cache helpers. */
uint64_t media_browse_uid_lookup(const char *uri);
void media_browse_uid_insert(const char *uri, uint64_t uid);
unsigned media_browse_uid_cache_size(void);
void media_browse_uid_cache_clear(void);

/* Check that a reply carries exactly the expected signature. */
bool media_browse_check_signature(DBusMessage *reply, const char *expected);

#endif /* BLUEZ_MEDIA_BROWSE_H */
