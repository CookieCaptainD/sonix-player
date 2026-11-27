/*
 * D-Bus simulation harness for media-browse.
 *
 * run-dbus-sim.sh starts a private session bus.  This program opens a client
 * connection through media-browse and a second connection that pretends to be
 * an MPRIS player, then checks the async calls, signature validation, per-
 * destination cancellation and the UID cache.
 */

#include <string.h>
#include <pthread.h>

#include "common.h"
#include "media-browse.h"

#define GOOD_NAME "org.mpris.MediaPlayer2.SimTest"
#define BAD_NAME  "org.mpris.MediaPlayer2.Bad"

static int items_calls, error_calls, done_calls;
static uint32_t reported_total;
static char last_error[128];

static void on_items(GSList *items, uint32_t total, void *user_data)
{
	items_calls++;
	reported_total = total;
	g_slist_free_full(items, g_free);
}

static void on_error(unsigned id, const char *dest, const char *msg,
								void *user_data)
{
	error_calls++;
	g_strlcpy(last_error, msg, sizeof(last_error));
}

static void on_done(unsigned id, bool success, void *user_data)
{
	done_calls++;
}

static void append_dict_string(DBusMessageIter *array, const char *key,
								const char *value)
{
	DBusMessageIter entry, var;

	dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL,
									&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(array, &entry);
}

static DBusConnection *pump_conn;
static volatile int pump_stop;

static void *pump_thread(void *unused)
{
	(void)unused;
	while (!pump_stop)
		dbus_connection_read_write_dispatch(pump_conn, 20);
	return NULL;
}

static DBusHandlerResult service_filter(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBusMessage *reply;
	DBusMessageIter root, array, item;
	const char *member;
	int i;

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	member = dbus_message_get_member(msg);

	if (strcmp(member, "GetAll") == 0 &&
		strcmp(dbus_message_get_interface(msg), "org.freedesktop.DBus.Properties") == 0) {
		DBusMessageIter in;
		const char *requested = NULL;
		if (dbus_message_iter_init(msg, &in) &&
			dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&in, &requested);
		if (requested == NULL || strcmp(requested, "org.bluez.MediaFolder1") != 0)
			goto generic_get_all;
		DBusMessageIter d, e;
		uint32_t total = 42;
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &d);
		dbus_message_iter_open_container(&d, DBUS_TYPE_ARRAY, "{sv}", &e);
		const char *key = "NumberOfItems";
		DBusMessageIter entry, value;
		dbus_message_iter_open_container(&e, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &value);
		dbus_message_iter_append_basic(&value, DBUS_TYPE_UINT32, &total);
		dbus_message_iter_close_container(&entry, &value);
		dbus_message_iter_close_container(&e, &entry);
		dbus_message_iter_close_container(&d, &e);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(dbus_message_get_destination(msg), BAD_NAME) == 0) {
		const char *wrong = "wrong";
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &wrong,
						DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "GetTracksMetadata") == 0) {
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &root);
		dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "a{sv}",
									&array);
		for (i = 0; i < 2; i++) {
			char title[32];

			snprintf(title, sizeof(title), "Track %d", i + 1);
			dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY,
							"{sv}", &item);
			append_dict_string(&item, "xesam:title", title);
			append_dict_string(&item, "xesam:url",
					i == 0 ? "file:///music/a.flac" :
							"file:///music/b.mp3");
			dbus_message_iter_close_container(&array, &item);
		}
		dbus_message_iter_close_container(&root, &array);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

generic_get_all:
	if (strcmp(member, "GetAll") == 0) {
		DBusMessageIter dict, entry, var, tracks;
		const char *key = "Tracks";

		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &root);
		dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}",
									&dict);
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY,
									NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
							"ao", &var);
		dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "o",
									&tracks);
		for (i = 0; i < 2; i++) {
			const char *path = i == 0 ?
				"/org/mpris/MediaPlayer2/Track/0" :
				"/org/mpris/MediaPlayer2/Track/1";

			dbus_message_iter_append_basic(&tracks,
					DBUS_TYPE_OBJECT_PATH, &path);
		}
		dbus_message_iter_close_container(&var, &tracks);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(&dict, &entry);
		dbus_message_iter_close_container(&root, &dict);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "OpenUri") == 0 || strcmp(member, "GoTo") == 0) {
		reply = dbus_message_new_method_return(msg);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void pump(DBusConnection *client, DBusConnection *service, int ms)
{
	int i;

	for (i = 0; i < ms; i++) {
		dbus_connection_read_write_dispatch(service, 1);
		dbus_connection_read_write_dispatch(client, 1);
	}
}

static DBusConnection *connect_bus(void)
{
	DBusError err;
	DBusConnection *conn;

	dbus_error_init(&err);
	conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (conn == NULL) {
		fprintf(stderr, "D-Bus connect failed: %s\n", err.message);
		dbus_error_free(&err);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	return conn;
}

int main(void)
{
	DBusConnection *client, *service;
	DBusError err;
	struct media_browse_cb cb = {
		.items = on_items, .error = on_error, .done = on_done,
	};
	struct media_browse *mb;
	unsigned id;
	int baseline_errors;

	client = connect_bus();
	service = connect_bus();
	if (client == NULL || service == NULL)
		return 1;

	dbus_error_init(&err);
	CHECK(dbus_bus_request_name(service, GOOD_NAME, 0, &err) ==
					DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
		dbus_bus_request_name(service, BAD_NAME, 0, &err) ==
					DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER,
						"service names acquired");
	dbus_error_free(&err);

	dbus_connection_add_filter(service, service_filter, NULL, NULL);

	mb = media_browse_new(client, &cb, NULL);

	media_browse_uid_cache_clear();
	id = media_browse_list_items(mb, GOOD_NAME, "/org/mpris/MediaPlayer2",
							0, 1);
	CHECK(id != 0, "list_items returns a call id");
	pump(client, service, 30);
	CHECK(items_calls == 1, "items callback fired once");
	CHECK(reported_total == 2, "two items reported");
	CHECK(media_browse_count(mb) == 2, "last count is two");
	CHECK(media_browse_uid_cache_size() == 2, "two UIDs cached");
	CHECK(media_browse_uid_lookup("file:///music/a.flac") != 0,
						"UID lookup finds cached URI");

	id = media_browse_get_count(mb, GOOD_NAME, "/org/mpris/MediaPlayer2");
	CHECK(id != 0, "get_count returns a call id");
	pump(client, service, 20);

	media_browse_get_all(mb, GOOD_NAME, "/org/mpris/MediaPlayer2");
	pump(client, service, 20);

	id = media_browse_change_folder(mb, GOOD_NAME, "/org/mpris/MediaPlayer2",
							42);
	CHECK(id != 0, "change_folder returns a call id");
	pump(client, service, 20);

	id = media_browse_play(mb, GOOD_NAME, "file:///music/a.flac");
	CHECK(id != 0, "play returns a call id");
	pump(client, service, 20);

	/* Per destination cancel: a call to a destination with no service must
	 * vanish without producing a callback. */
	baseline_errors = error_calls;
	id = media_browse_list_items(mb, "org.mpris.MediaPlayer2.Gone",
							"/org/mpris/MediaPlayer2", 0, 1);
	CHECK(id != 0, "cancelled call gets an id");
	media_browse_cancel_dest(mb, "org.mpris.MediaPlayer2.Gone");
	pump(client, service, 20);
	CHECK(error_calls == baseline_errors, "cancelled call has no error");

	/* Signature validation: BAD_NAME returns a string instead of aa{sv}. */
	id = media_browse_list_items(mb, BAD_NAME, "/org/mpris/MediaPlayer2",
							0, 1);
	CHECK(id != 0, "bad-signature call gets an id");
	pump(client, service, 20);
	CHECK(error_calls == baseline_errors + 1 &&
			strstr(last_error, "signature") != NULL,
						"invalid signature reported");

	/* LRU eviction. */
	media_browse_uid_cache_clear();
	for (int i = 0; i < MEDIA_BROWSE_UID_CACHE + 1; i++) {
		char uri[64];

		snprintf(uri, sizeof(uri), "file:///track/%d", i);
		media_browse_uid_insert(uri, i + 1);
	}
	CHECK(media_browse_uid_cache_size() == MEDIA_BROWSE_UID_CACHE,
						"UID cache capped at 512");

	pump_conn = service;
	pthread_t pump;
	pthread_create(&pump, NULL, pump_thread, NULL);
	CHECK(media_browse_count_sync(mb, GOOD_NAME,
			"/org/mpris/MediaPlayer2/Filesystem") == 42, "sync folder count");
	pump_stop = 1;
	pthread_join(pump, NULL);

	media_browse_free(mb);
	dbus_connection_close(client);
	dbus_connection_close(service);
	dbus_connection_unref(client);
	dbus_connection_unref(service);

	TEST_REPORT("media-browse");
}
