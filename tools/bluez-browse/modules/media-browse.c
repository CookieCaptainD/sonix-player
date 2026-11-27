/*
 * Asynchronous MPRIS browsing bridge used by the AVRCP target.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media-browse.h"
#include "avrcp-tg-browse.h"

enum call_kind {
	CALL_LIST,
	CALL_GET_ALL,
	CALL_COUNT,
	CALL_CHANGE,
	CALL_PLAY,
};

struct browse_call {
	unsigned id;
	enum call_kind kind;
	char *dest;
	char *path;
	char *arg;
	uint64_t uid;
	uint32_t start;
	uint32_t end;
	int retries;
	uint64_t stamp;
	DBusPendingCall *pending;
	struct media_browse *mb;
};

struct media_browse {
	DBusConnection *conn;
	struct media_browse_cb cb;
	void *user_data;
	GSList *calls;
	unsigned next_id;
	uint32_t last_count;
};

struct uid_entry {
	char *uri;
	uint64_t uid;
};

static GHashTable *uid_map;
static GSList *uid_lru;

/* ------------------------------------------------------------------ */
/* UID cache: 512 entries, least recently used evicted first.          */
/* ------------------------------------------------------------------ */

uint64_t media_browse_uid_lookup(const char *uri)
{
	struct uid_entry *e;

	if (uri == NULL || uid_map == NULL)
		return 0;

	e = g_hash_table_lookup(uid_map, uri);
	if (e == NULL)
		return 0;

	/* Touch: move to the front of the LRU list. */
	uid_lru = g_slist_remove(uid_lru, e);
	uid_lru = g_slist_prepend(uid_lru, e);

	return e->uid;
}

void media_browse_uid_insert(const char *uri, uint64_t uid)
{
	struct uid_entry *e;

	if (uri == NULL || *uri == '\0')
		return;

	if (uid_map == NULL) {
		uid_map = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);
		uid_lru = NULL;
	}

	e = g_hash_table_lookup(uid_map, uri);
	if (e != NULL) {
		e->uid = uid;
		uid_lru = g_slist_remove(uid_lru, e);
		uid_lru = g_slist_prepend(uid_lru, e);
		return;
	}

	e = g_new0(struct uid_entry, 1);
	e->uri = g_strdup(uri);
	e->uid = uid;
	g_hash_table_insert(uid_map, e->uri, e);
	uid_lru = g_slist_prepend(uid_lru, e);

	while (g_slist_length(uid_lru) > MEDIA_BROWSE_UID_CACHE) {
		GSList *last = g_slist_last(uid_lru);
		struct uid_entry *old = last->data;

		g_hash_table_remove(uid_map, old->uri);
		uid_lru = g_slist_remove_link(uid_lru, last);
		g_slist_free1(last);
	}
}

unsigned media_browse_uid_cache_size(void)
{
	return uid_map != NULL ? g_hash_table_size(uid_map) : 0;
}

void media_browse_uid_cache_clear(void)
{
	if (uid_map != NULL)
		g_hash_table_remove_all(uid_map);
	g_slist_free(uid_lru);
	uid_lru = NULL;
}

bool media_browse_check_signature(DBusMessage *reply, const char *expected)
{
	const char *got;

	if (reply == NULL || expected == NULL)
		return false;

	got = dbus_message_get_signature(reply);

	return got != NULL && strcmp(got, expected) == 0;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

const char *media_browse_parent(const char *path, char *buf, size_t len)
{
	char *slash;

	if (path == NULL || buf == NULL || len == 0)
		return NULL;

	g_strlcpy(buf, path, len);
	slash = strrchr(buf, '/');
	if (slash == NULL || slash == buf)
		return buf;

	*slash = '\0';

	return buf;
}

/* ------------------------------------------------------------------ */
/* Reply parsing                                                       */
/* ------------------------------------------------------------------ */

/* The iterator passed here already points inside the variant. */
static void read_variant_string(DBusMessageIter *var, const char **out)
{
	const char *s = NULL;

	if (dbus_message_iter_get_arg_type(var) == DBUS_TYPE_STRING)
		dbus_message_iter_get_basic(var, &s);

	*out = s;
}

static uint64_t uri_uid(const char *uri)
{
	uint64_t uid = media_browse_uid_lookup(uri);

	if (uid != 0)
		return uid;

	uid = g_str_hash(uri);
	uid = (uid << 32) | (uint32_t) g_str_hash(uri);
	if (uid == 0)
		uid = 1;
	media_browse_uid_insert(uri, uid);

	return uid;
}

static GSList *parse_item_array(DBusMessage *msg)
{
	DBusMessageIter root, array, item;
	GSList *items = NULL;

	if (!dbus_message_iter_init(msg, &root) ||
			dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY)
		return NULL;

	dbus_message_iter_recurse(&root, &array);
	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_ARRAY) {
		struct media_browse_item *it;
		const char *title = NULL, *url = NULL, *mime = NULL;

		dbus_message_iter_recurse(&array, &item);

		while (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_DICT_ENTRY) {
			const char *key;
			DBusMessageIter entry, var;

			dbus_message_iter_recurse(&item, &entry);
			if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
				break;
			dbus_message_iter_get_basic(&entry, &key);
			if (!dbus_message_iter_next(&entry))
				break;
			dbus_message_iter_recurse(&entry, &var);

			if (strcmp(key, "xesam:title") == 0 ||
					strcmp(key, "title") == 0)
				read_variant_string(&var, &title);
			else if (strcmp(key, "xesam:url") == 0 ||
					strcmp(key, "url") == 0)
				read_variant_string(&var, &url);
			else if (strcmp(key, "mimeType") == 0)
				read_variant_string(&var, &mime);

			dbus_message_iter_next(&item);
		}

		it = g_new0(struct media_browse_item, 1);
		it->uri = g_strdup(url != NULL ? url : "");
		it->uid = uri_uid(it->uri);
		it->mime = g_strdup(mime != NULL ? mime : "");
		it->type = tg_map_item_type(it->uri, it->mime, false);
		it->name = g_strdup(title != NULL ? title : it->uri);
		it->playable = true;
		items = g_slist_prepend(items, it);

		dbus_message_iter_next(&array);
	}

	return g_slist_reverse(items);
}

static uint32_t parse_tracks_count(DBusMessage *msg)
{
	DBusMessageIter root, dict, entry, var, array;
	uint32_t n = 0;

	if (!dbus_message_iter_init(msg, &root) ||
			dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY)
		return 0;

	dbus_message_iter_recurse(&root, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		const char *key;

		dbus_message_iter_recurse(&dict, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			break;
		dbus_message_iter_get_basic(&entry, &key);
		if (strcmp(key, "Tracks") != 0 || !dbus_message_iter_next(&entry)) {
			dbus_message_iter_next(&dict);
			continue;
		}
		dbus_message_iter_recurse(&entry, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&var, &array);
			while (dbus_message_iter_get_arg_type(&array) !=
							DBUS_TYPE_INVALID) {
				n++;
				dbus_message_iter_next(&array);
			}
		}
		break;
	}

	return n;
}

static void call_free(struct browse_call *call)
{
	if (call == NULL)
		return;

	if (call->pending != NULL) {
		dbus_pending_call_cancel(call->pending);
		dbus_pending_call_unref(call->pending);
	}
	g_free(call->dest);
	g_free(call->path);
	g_free(call->arg);
	g_free(call);
}

static void report_error(struct browse_call *call, const char *message)
{
	struct media_browse *mb = call->mb;

	if (mb->cb.error != NULL)
		mb->cb.error(call->id, call->dest, message, mb->user_data);
	if (mb->cb.done != NULL)
		mb->cb.done(call->id, false, mb->user_data);
}

static void report_done(struct browse_call *call, bool success)
{
	struct media_browse *mb = call->mb;

	if (mb->cb.done != NULL)
		mb->cb.done(call->id, success, mb->user_data);
}

static void handle_reply(struct browse_call *call, DBusMessage *reply)
{
	struct media_browse *mb = call->mb;

	if (reply != NULL && dbus_message_get_type(reply) ==
						DBUS_MESSAGE_TYPE_ERROR) {
		report_error(call, dbus_message_get_error_name(reply) != NULL ?
				dbus_message_get_error_name(reply) : "D-Bus error");
		return;
	}

	switch (call->kind) {
	case CALL_LIST: {
		GSList *items;

		if (!media_browse_check_signature(reply, "aa{sv}")) {
			report_error(call, "Bad GetTracksMetadata signature");
			break;
		}

		items = parse_item_array(reply);
		mb->last_count = g_slist_length(items);
		if (mb->cb.items != NULL)
			mb->cb.items(items, mb->last_count, mb->user_data);
		else
			g_slist_free_full(items, (GDestroyNotify) g_free);
		report_done(call, true);
		break;
	}
	case CALL_COUNT:
		if (!media_browse_check_signature(reply, "a{sv}")) {
			report_error(call, "Bad Tracks signature");
			break;
		}
		mb->last_count = parse_tracks_count(reply);
		report_done(call, true);
		break;
	case CALL_GET_ALL:
		if (!media_browse_check_signature(reply, "a{sv}")) {
			report_error(call, "Bad GetAll signature");
			break;
		}
		mb->last_count = parse_tracks_count(reply);
		report_done(call, true);
		break;
	case CALL_CHANGE:
		/* The app-side MediaFolder1.ChangeFolder replies (uu)
		 * (count, position); older players reply with no arguments.
		 * Accept both and expose the count when present. */
		if (media_browse_check_signature(reply, "uu")) {
			DBusMessageIter iter;
			uint32_t count = 0;

			if (dbus_message_iter_init(reply, &iter) &&
				dbus_message_iter_get_arg_type(&iter) ==
								DBUS_TYPE_UINT32) {
				dbus_message_iter_get_basic(&iter, &count);
				mb->last_count = count;
			}
		} else if (!media_browse_check_signature(reply, "")) {
			report_error(call, "Bad reply signature");
			break;
		}
		report_done(call, true);
		break;
	case CALL_PLAY:
		if (!media_browse_check_signature(reply, "")) {
			report_error(call, "Bad reply signature");
			break;
		}
		report_done(call, true);
		break;
	}
}

static void reply_cb(DBusPendingCall *pending, void *user_data)
{
	struct browse_call *call = user_data;
	DBusMessage *reply;

	if (call->mb == NULL || call->pending != pending)
		return;

	reply = dbus_pending_call_steal_reply(pending);
	handle_reply(call, reply);
	if (reply != NULL)
		dbus_message_unref(reply);

	call->mb->calls = g_slist_remove(call->mb->calls, call);
	call_free(call);
}

static bool send_call(struct media_browse *mb, struct browse_call *call)
{
	DBusMessage *msg;
	const char *iface = MEDIA_BROWSE_MPRIS_TRACKLIST;

	switch (call->kind) {
	case CALL_LIST:
		msg = dbus_message_new_method_call(call->dest, call->path,
				iface, "GetTracksMetadata");
		break;
	case CALL_GET_ALL:
	case CALL_COUNT:
		msg = dbus_message_new_method_call(call->dest, call->path,
				MEDIA_BROWSE_PROPERTIES, "GetAll");
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
						DBUS_TYPE_INVALID);
		break;
	case CALL_CHANGE:
		msg = dbus_message_new_method_call(call->dest, call->path,
				iface, "GoTo");
		dbus_message_append_args(msg, DBUS_TYPE_UINT64, &call->uid,
						DBUS_TYPE_INVALID);
		break;
	case CALL_PLAY:
		msg = dbus_message_new_method_call(call->dest, call->path,
				MEDIA_BROWSE_MPRIS_PLAYER, "OpenUri");
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &call->arg,
						DBUS_TYPE_INVALID);
		break;
	default:
		return false;
	}

	if (msg == NULL)
		return false;

	if (call->kind == CALL_LIST) {
		DBusMessageIter iter, array;
		char paths[32][96];
		uint32_t i, n = 0;

		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &array);
		for (i = call->start; i <= call->end && n < 32; i++, n++) {
			const char *p = paths[n];

			snprintf(paths[n], sizeof(paths[n]),
					"/org/mpris/MediaPlayer2/Track/%u", i);
			dbus_message_iter_append_basic(&array,
					DBUS_TYPE_OBJECT_PATH, &p);
		}
		dbus_message_iter_close_container(&iter, &array);
	}

	dbus_connection_send_with_reply(mb->conn, msg, &call->pending,
							MEDIA_BROWSE_TIMEOUT_MS);
	dbus_message_unref(msg);

	if (call->pending == NULL)
		return false;

	call->stamp = g_get_monotonic_time();

	return dbus_pending_call_set_notify(call->pending, reply_cb, call, NULL);
}

static unsigned new_call(struct media_browse *mb, enum call_kind kind,
					const char *dest, const char *path,
					uint32_t start, uint32_t end, uint64_t uid,
					const char *arg)
{
	struct browse_call *call;

	if (mb == NULL || mb->conn == NULL || dest == NULL || path == NULL)
		return 0;

	call = g_new0(struct browse_call, 1);
	call->id = ++mb->next_id;
	call->kind = kind;
	call->dest = g_strdup(dest);
	call->path = g_strdup(path);
	call->arg = g_strdup(arg != NULL ? arg : "");
	call->start = start;
	call->end = end;
	call->uid = uid;
	call->mb = mb;

	if (!send_call(mb, call)) {
		call_free(call);
		return 0;
	}

	mb->calls = g_slist_prepend(mb->calls, call);

	return call->id;
}

unsigned media_browse_list_items(struct media_browse *mb, const char *dest,
					const char *path, uint32_t start,
					uint32_t end)
{
	return new_call(mb, CALL_LIST, dest, path, start, end, 0, NULL);
}

unsigned media_browse_get_all(struct media_browse *mb, const char *dest,
					const char *path)
{
	return new_call(mb, CALL_GET_ALL, dest, path, 0, 0, 0, NULL);
}

unsigned media_browse_get_count(struct media_browse *mb, const char *dest,
					const char *path)
{
	return new_call(mb, CALL_COUNT, dest, path, 0, 0, 0, NULL);
}

uint32_t media_browse_count_sync(struct media_browse *mb, const char *dest,
					const char *path)
{
	DBusMessage *msg, *reply;
	DBusMessageIter args;
	DBusError err;
	uint32_t count = 0;
	const char *iface = "org.bluez.MediaFolder1";

	if (mb == NULL || mb->conn == NULL || dest == NULL || path == NULL)
		return 0;

	msg = dbus_message_new_method_call(dest, path, MEDIA_BROWSE_PROPERTIES,
							"GetAll");
	if (msg == NULL)
		return 0;
	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface);

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(mb->conn, msg,
							MEDIA_BROWSE_TIMEOUT_MS, &err);
	dbus_message_unref(msg);
	if (reply == NULL) {
		dbus_error_free(&err);
		return 0;
	}
	if (media_browse_check_signature(reply, "a{sv}")) {
		DBusMessageIter top, dict;
		if (dbus_message_iter_init(reply, &top) &&
			dbus_message_iter_get_arg_type(&top) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&top, &dict);
			while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
				DBusMessageIter entry, value;
				const char *key = NULL;
				dbus_message_iter_recurse(&dict, &entry);
				dbus_message_iter_get_basic(&entry, &key);
				dbus_message_iter_next(&entry);
				if (key && strcmp(key, "NumberOfItems") == 0 &&
					dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
					dbus_message_iter_recurse(&entry, &value);
					if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT32)
						dbus_message_iter_get_basic(&value, &count);
					break;
				}
				dbus_message_iter_next(&dict);
			}
		}
	}
	dbus_message_unref(reply);
	return count;
}

unsigned media_browse_change_folder(struct media_browse *mb, const char *dest,
					const char *path, uint64_t uid)
{
	return new_call(mb, CALL_CHANGE, dest, path, 0, 0, uid, NULL);
}

unsigned media_browse_play(struct media_browse *mb, const char *dest,
					const char *uri)
{
	return new_call(mb, CALL_PLAY, dest, "/org/mpris/MediaPlayer2",
					0, 0, 0, uri);
}

void media_browse_cancel_dest(struct media_browse *mb, const char *dest)
{
	GSList *l;

	if (mb == NULL || dest == NULL)
		return;

	for (l = mb->calls; l != NULL;) {
		GSList *next = l->next;
		struct browse_call *call = l->data;

		if (call != NULL && strcmp(call->dest, dest) == 0) {
			mb->calls = g_slist_remove_link(mb->calls, l);
			call_free(call);
			g_slist_free1(l);
		}

		l = next;
	}
}

void media_browse_dispatch(struct media_browse *mb, int timeout_ms)
{
	GSList *l;

	if (mb == NULL || mb->conn == NULL)
		return;

	dbus_connection_read_write(mb->conn, 0);
	dbus_connection_dispatch(mb->conn);

	for (l = mb->calls; l != NULL; l = l->next) {
		struct browse_call *call = l->data;
		uint64_t elapsed;

		if (call == NULL || call->pending == NULL ||
				dbus_pending_call_get_completed(call->pending))
			continue;

		elapsed = (g_get_monotonic_time() - call->stamp) / 1000;
		if (elapsed < (uint64_t) MEDIA_BROWSE_TIMEOUT_MS)
			continue;

		if (call->retries >= MEDIA_BROWSE_MAX_RETRIES) {
			report_error(call, "Timeout");
			dbus_pending_call_cancel(call->pending);
			call->pending = NULL;
			mb->calls = g_slist_remove(mb->calls, call);
			call_free(call);
			break;
		}

		/* One retry with the same destination and arguments. */
		dbus_pending_call_cancel(call->pending);
		dbus_pending_call_unref(call->pending);
		call->pending = NULL;
		call->retries++;
		send_call(mb, call);
	}

	if (timeout_ms > 0)
		dbus_connection_read_write_dispatch(mb->conn, timeout_ms);
}

uint32_t media_browse_count(struct media_browse *mb)
{
	return mb != NULL ? mb->last_count : 0;
}

struct media_browse *media_browse_new(DBusConnection *conn,
				const struct media_browse_cb *cb, void *user_data)
{
	struct media_browse *mb;

	if (conn == NULL)
		return NULL;

	mb = g_new0(struct media_browse, 1);
	mb->conn = conn;
	mb->user_data = user_data;
	if (cb != NULL)
		mb->cb = *cb;

	return mb;
}

void media_browse_free(struct media_browse *mb)
{
	GSList *l;

	if (mb == NULL)
		return;

	for (l = mb->calls; l != NULL; l = l->next)
		call_free(l->data);
	g_slist_free(mb->calls);
	g_free(mb);
}
