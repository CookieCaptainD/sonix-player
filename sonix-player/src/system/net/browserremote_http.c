/*
 * Browser remote HTTP service.
 *
 * A small, single-threaded-per-connection server on its own port. It serves
 * one embedded page and a JSON API. Access to the core is guarded by a mutex;
 * the player operations below run on the server thread and only call the same
 * public player API the interface uses.
 */

#include "browserremote_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/library/library.h"
#include "src/system/net/browserremote.h"
#include "src/system/net/remote_page.h"
#include "src/system/net/wifi.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

#define REQ_MAX 16384
#define RESP_MAX (REQ_MAX + 4096)

static browserremote_ctx_t g_ctx;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static volatile bool g_running;
static volatile bool g_stop;
static bool g_enabled;
static bool g_thread_created;

// ---------------------------------------------------------------------------
// Player operations, wired once in browserremote_http_init()
// ---------------------------------------------------------------------------

static void host_state(browserremote_state_t *out, void *user)
{
	(void)user;
	device_state_t st;
	memset(&st, 0, sizeof(st));
	device_state_get(&st);

	out->status = (st.status == AUDIO_STATUS_PLAYING)   ? BR_STATUS_PLAYING
				  : (st.status == AUDIO_STATUS_PAUSED) ? BR_STATUS_PAUSED
													  : BR_STATUS_STOPPED;
	snprintf(out->path, sizeof(out->path), "%s", st.current_file);
	snprintf(out->title, sizeof(out->title), "%s", st.metadata.title ? st.metadata.title : "");
	snprintf(out->artist, sizeof(out->artist), "%s", st.metadata.artist ? st.metadata.artist : "");
	out->position = st.progress_current_secs;
	out->duration = st.progress_total_secs;
	out->volume = device_state_get_volume_percent();
	out->mode = (int)playlist_get_mode();
	out->queue_count = playlist_count();
	out->queue_index = playlist_current_index();
	out->favourite = st.current_file[0] ? library_fav_contains(st.current_file) : false;
}

static int host_queue_count(void *user)
{
	(void)user;
	return playlist_count();
}

static void item_from_path(const char *path, const char *name, const char *artist, browserremote_item_t *out)
{
	snprintf(out->path, sizeof(out->path), "%s", path ? path : "");
	snprintf(out->title, sizeof(out->title), "%s", name ? name : "");
	snprintf(out->artist, sizeof(out->artist), "%s", artist ? artist : "");
}

static bool host_queue_item(int index, browserremote_item_t *out, void *user)
{
	(void)user;
	char path[BR_PATH_LEN];
	if (!playlist_path_at(index, path, sizeof(path))) {
		return false;
	}
	const char *base = strrchr(path, '/');
	item_from_path(path, base ? base + 1 : path, "", out);
	return true;
}

typedef struct {
	int offset;
	int count;
	int delivered;
	browserremote_item_t *out;
} walk_t;

static bool take_row(const char *name, const char *path, const char *artist, void *user)
{
	walk_t *w = user;
	if (w->delivered >= w->count) {
		return false;
	}
	if (w->offset > 0) {
		w->offset--;
		return true;
	}
	item_from_path(path ? path : "", name, artist, w->out + w->delivered);
	w->delivered++;
	return true;
}

static void search_row(library_search_type_t type, const char *name, const char *path, void *user)
{
	(void)type;
	walk_t *w = user;
	if (w->delivered >= w->count) {
		return;
	}
	if (w->offset > 0) {
		w->offset--;
		return;
	}
	item_from_path(path ? path : "", name, "", w->out + w->delivered);
	w->delivered++;
}

static int host_library(const char *query, int offset, int count, browserremote_item_t *out, int max, void *user)
{
	(void)user;
	if (count > max) {
		count = max;
	}
	if (count <= 0) {
		return 0;
	}
	walk_t w = {.offset = offset < 0 ? 0 : offset, .count = count, .delivered = 0, .out = out};

	if (query != NULL && strcmp(query, "@fav") == 0) {
		library_for_each(LIBRARY_LIST_FAVOURITES, LIBRARY_FILTER_NONE, NULL, take_row, &w);
	} else if (query != NULL && query[0] != '\0') {
		library_search(query, offset + count, search_row, &w);
	} else {
		library_for_each(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_NONE, NULL, take_row, &w);
	}
	return w.delivered;
}

static bool host_fav_contains(const char *path, void *user)
{
	(void)user;
	return path[0] ? library_fav_contains(path) : false;
}

static bool host_fav_toggle(const char *path, const char *title, const char *artist, void *user)
{
	(void)user;
	return library_fav_toggle(path, title, artist);
}

static void host_play_pause(void *user) { (void)user; device_state_toggle_play_pause(); }
static void host_next(void *user) { (void)user; device_state_next(NULL, 0); }
static void host_prev(void *user) { (void)user; device_state_prev(NULL, 0); }
static void host_seek(double seconds, void *user)
{
	(void)user;
	device_state_seek(seconds);
}
static void host_volume(int percent, void *user)
{
	(void)user;
	int want = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
	device_state_change_volume_percent(want - device_state_get_volume_percent());
}
static void host_mode(void *user) { (void)user; playlist_cycle_mode(); }
static void host_play_path(const char *path, void *user)
{
	(void)user;
	device_state_play_file(path);
}
static void host_play_index(int index, void *user)
{
	(void)user;
	device_state_play_queue_index(index);
}

static const browserremote_ops_t g_ops = {
	.state = host_state,
	.queue_count = host_queue_count,
	.queue_item = host_queue_item,
	.library = host_library,
	.fav_contains = host_fav_contains,
	.fav_toggle = host_fav_toggle,
	.play_pause = host_play_pause,
	.next = host_next,
	.prev = host_prev,
	.seek = host_seek,
	.volume = host_volume,
	.mode = host_mode,
	.play_path = host_play_path,
	.play_index = host_play_index,
};

// ---------------------------------------------------------------------------
// Small parsing helpers
// ---------------------------------------------------------------------------

static bool cookie_value(const char *cookies, const char *name, char *out, size_t out_size)
{
	out[0] = '\0';
	if (cookies == NULL) {
		return false;
	}
	const char *p = cookies;
	while (*p != '\0') {
		while (*p == ' ') {
			p++;
		}
		size_t len = strlen(name);
		if (strncmp(p, name, len) == 0 && p[len] == '=') {
			p += len + 1;
			size_t i = 0;
			while (*p != '\0' && *p != ';' && *p != ' ' && i + 1 < out_size) {
				out[i++] = *p++;
			}
			out[i] = '\0';
			return true;
		}
		while (*p != '\0' && *p != ';') {
			p++;
		}
		if (*p == ';') {
			p++;
		}
	}
	return false;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static void url_decode(const char *in, char *out, size_t out_size)
{
	size_t i = 0;
	while (*in != '\0' && i + 1 < out_size) {
		if (*in == '%' && hex_val(in[1]) >= 0 && hex_val(in[2]) >= 0) {
			out[i++] = (char)(hex_val(in[1]) * 16 + hex_val(in[2]));
			in += 3;
		} else if (*in == '+') {
			out[i++] = ' ';
			in++;
		} else {
			out[i++] = *in++;
		}
	}
	out[i] = '\0';
}

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return p;
}

static bool json_string(const char *json, const char *key, char *out, size_t out_size)
{
	out[0] = '\0';
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	const char *p = strstr(json, needle);
	if (p == NULL) {
		return false;
	}
	p = skip_ws(p + strlen(needle));
	if (*p != ':') {
		return false;
	}
	p = skip_ws(p + 1);
	if (*p != '"') {
		return false;
	}
	p++;
	size_t i = 0;
	while (*p != '\0' && *p != '"' && i + 1 < out_size) {
		if (*p == '\\' && p[1] != '\0') {
			p++;
			switch (*p) {
			case 'n':
				out[i++] = '\n';
				break;
			case 'r':
				out[i++] = '\r';
				break;
			case 't':
				out[i++] = '\t';
				break;
			default:
				out[i++] = *p;
				break;
			}
			p++;
		} else {
			out[i++] = *p++;
		}
	}
	out[i] = '\0';
	return *p == '"';
}

static bool json_number(const char *json, const char *key, double *out)
{
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	const char *p = strstr(json, needle);
	if (p == NULL) {
		return false;
	}
	p = skip_ws(p + strlen(needle));
	if (*p != ':') {
		return false;
	}
	p = skip_ws(p + 1);
	char *end = NULL;
	double v = strtod(p, &end);
	if (end == p) {
		return false;
	}
	*out = v;
	return true;
}

// ---------------------------------------------------------------------------
// HTTP responses
// ---------------------------------------------------------------------------

static void send_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, data, len, 0);
		if (n <= 0) {
			return;
		}
		data += n;
		len -= (size_t)n;
	}
}

static void reply(int fd, int code, const char *status, const char *type, const char *body,
				  const char *extra_headers)
{
	char headers[1024];
	int n = snprintf(headers, sizeof(headers),
					 "HTTP/1.1 %d %s\r\n"
					 "Content-Type: %s; charset=utf-8\r\n"
					 "Content-Length: %zu\r\n"
					 "Cache-Control: no-store\r\n"
					 "Connection: close\r\n"
					 "%s\r\n",
					 code, status, type, strlen(body), extra_headers ? extra_headers : "");
	if (n < 0 || (size_t)n >= sizeof(headers)) {
		return;
	}
	send_all(fd, headers, strlen(headers));
	send_all(fd, body, strlen(body));
}

static void reply_json(int fd, int code, const char *status, const char *body, const char *extra_headers)
{
	reply(fd, code, status, "application/json", body, extra_headers);
}

static void browser_cookie(const char *browser_id, char *out, size_t out_size)
{
	snprintf(out, out_size, "Set-Cookie: br_browser=%s; Path=/; SameSite=Strict\r\n", browser_id);
}

static void new_browser_id(char *out, size_t out_size)
{
	snprintf(out, out_size, "%08llx%08llx", (unsigned long long)time(NULL),
			 (unsigned long long)(uintptr_t)out ^ (unsigned long long)pthread_self());
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

static void do_challenge(int fd, const char *browser_id, char *cookie_header, size_t cookie_size)
{
	pthread_mutex_lock(&g_lock);
	if (!browserremote_has_pin(&g_ctx)) {
		pthread_mutex_unlock(&g_lock);
		reply_json(fd, 403, "Forbidden", "{\"error\":\"no_pin\"}", cookie_header);
		return;
	}
	char nonce[BR_NONCE_LEN];
	snprintf(nonce, sizeof(nonce), "%s", browserremote_challenge(&g_ctx, browser_id, time(NULL)));
	pthread_mutex_unlock(&g_lock);

	char body[256];
	snprintf(body, sizeof(body), "{\"nonce\":\"%s\"}", nonce);
	reply_json(fd, 200, "OK", body, cookie_header);
}

static void do_login(int fd, const char *browser_id, const char *body, char *cookie_header, size_t cookie_size)
{
	char nonce[BR_NONCE_LEN];
	char pin[BR_PIN_MAX + 8];
	if (!json_string(body, "nonce", nonce, sizeof(nonce)) || !json_string(body, "pin", pin, sizeof(pin))) {
		reply_json(fd, 400, "Bad Request", "{\"error\":\"bad_request\"}", cookie_header);
		return;
	}

	char token[BR_TOKEN_LEN];
	pthread_mutex_lock(&g_lock);
	bool ok = browserremote_login(&g_ctx, browser_id, nonce, pin, token, sizeof(token), time(NULL));
	pthread_mutex_unlock(&g_lock);
	if (!ok) {
		reply_json(fd, 401, "Unauthorized", "{\"error\":\"pin\"}", cookie_header);
		return;
	}

	char set_session[256];
	snprintf(set_session, sizeof(set_session),
			 "Set-Cookie: br_session=%s; Path=/; SameSite=Strict; HttpOnly\r\n", token);
	char headers[512];
	snprintf(headers, sizeof(headers), "%s%s", cookie_header, set_session);
	reply_json(fd, 200, "OK", "{\"ok\":true}", headers);
}

static bool command_from_control(const char *body, browserremote_cmd_t *cmd)
{
	char name[32];
	if (!json_string(body, "command", name, sizeof(name))) {
		return false;
	}
	double num;
	if (strcmp(name, "play_pause") == 0) {
		cmd->command = BR_CMD_PLAY_PAUSE;
	} else if (strcmp(name, "next") == 0) {
		cmd->command = BR_CMD_NEXT;
	} else if (strcmp(name, "prev") == 0) {
		cmd->command = BR_CMD_PREV;
	} else if (strcmp(name, "mode") == 0) {
		cmd->command = BR_CMD_MODE;
	} else if (strcmp(name, "seek") == 0) {
		cmd->command = BR_CMD_SEEK;
		if (!json_number(body, "seconds", &num)) {
			return false;
		}
		cmd->seconds = num;
	} else if (strcmp(name, "volume") == 0) {
		cmd->command = BR_CMD_VOLUME;
		if (!json_number(body, "percent", &num)) {
			return false;
		}
		cmd->percent = (int)num;
	} else if (strcmp(name, "favourite") == 0) {
		cmd->command = BR_CMD_TOGGLE_FAV;
		json_string(body, "path", cmd->path, sizeof(cmd->path));
		json_string(body, "title", cmd->title, sizeof(cmd->title));
		json_string(body, "artist", cmd->artist, sizeof(cmd->artist));
	} else if (strcmp(name, "play_path") == 0) {
		cmd->command = BR_CMD_PLAY_PATH;
		if (!json_string(body, "path", cmd->path, sizeof(cmd->path))) {
			return false;
		}
	} else if (strcmp(name, "play_index") == 0) {
		cmd->command = BR_CMD_PLAY_INDEX;
		if (!json_number(body, "index", &num)) {
			return false;
		}
		cmd->index = (int)num;
	} else {
		return false;
	}
	return true;
}

static void query_param(const char *query, const char *key, char *out, size_t out_size)
{
	out[0] = '\0';
	if (query == NULL) {
		return;
	}
	const char *p = query;
	while (*p != '\0') {
		size_t len = strlen(key);
		if (strncmp(p, key, len) == 0 && p[len] == '=') {
			const char *start = p + len + 1;
			const char *end = strchr(start, '&');
			if (end == NULL) {
				end = start + strlen(start);
			}
			char raw[256];
			size_t n = (size_t)(end - start);
			if (n >= sizeof(raw)) {
				n = sizeof(raw) - 1;
			}
			memcpy(raw, start, n);
			raw[n] = '\0';
			url_decode(raw, out, out_size);
			return;
		}
		p = strchr(p, '&');
		if (p == NULL) {
			return;
		}
		p++;
	}
}

static void do_command(int fd, browserremote_cmd_t *cmd)
{
	char body[RESP_MAX];
	bool ok;
	pthread_mutex_lock(&g_lock);
	ok = browserremote_execute(&g_ctx, cmd, body, sizeof(body));
	pthread_mutex_unlock(&g_lock);
	if (!ok) {
		reply_json(fd, 400, "Bad Request", "{\"error\":\"bad_command\"}", NULL);
		return;
	}
	reply_json(fd, 200, "OK", body, NULL);
}

static void handle_request(int fd, char *request, size_t length)
{
	request[length] = '\0';

	char *line_end = strstr(request, "\r\n");
	if (line_end == NULL) {
		reply(fd, 400, "Bad Request", "text/plain", "bad request", NULL);
		return;
	}
	*line_end = '\0';
	char method[16] = {0};
	char target[1024] = {0};
	sscanf(request, "%15s %1023s", method, target);

	char *headers_start = line_end + 2;
	char *body = strstr(headers_start, "\r\n\r\n");
	const char *request_body = "";
	if (body != NULL) {
		request_body = body + 4;
	}

	char cookies[512] = {0};
	char *hp = headers_start;
	while (body != NULL && hp < body) {
		char *eol = strstr(hp, "\r\n");
		if (eol == NULL) {
			break;
		}
		if (strncmp(hp, "Cookie:", 7) == 0) {
			snprintf(cookies, sizeof(cookies), "%s", skip_ws(hp + 7));
			break;
		}
		hp = eol + 2;
	}

	char browser_id[BR_ID_LEN];
	if (!cookie_value(cookies, "br_browser", browser_id, sizeof(browser_id)) || browser_id[0] == '\0') {
		new_browser_id(browser_id, sizeof(browser_id));
	}
	char cookie_header[256];
	browser_cookie(browser_id, cookie_header, sizeof(cookie_header));

	char session[BR_TOKEN_LEN];
	cookie_value(cookies, "br_session", session, sizeof(session));

	char *query = strchr(target, '?');
	if (query != NULL) {
		*query++ = '\0';
	}

	pthread_mutex_lock(&g_lock);
	browserremote_touch(&g_ctx, time(NULL));
	pthread_mutex_unlock(&g_lock);

	if (strcmp(target, "/") == 0) {
		reply(fd, 200, "OK", "text/html", BROWSERREMOTE_PAGE, cookie_header);
		return;
	}
	if (strcmp(target, "/api/challenge") == 0) {
		do_challenge(fd, browser_id, cookie_header, sizeof(cookie_header));
		return;
	}
	if (strcmp(target, "/api/login") == 0) {
		do_login(fd, browser_id, request_body, cookie_header, sizeof(cookie_header));
		return;
	}

	bool authorised;
	pthread_mutex_lock(&g_lock);
	authorised = browserremote_session_valid(&g_ctx, browser_id, session, time(NULL));
	pthread_mutex_unlock(&g_lock);
	if (!authorised) {
		reply_json(fd, 401, "Unauthorized", "{\"error\":\"session\"}", cookie_header);
		return;
	}

	if (strcmp(target, "/api/logout") == 0) {
		pthread_mutex_lock(&g_lock);
		browserremote_logout(&g_ctx, session);
		pthread_mutex_unlock(&g_lock);
		reply_json(fd, 200, "OK", "{\"ok\":true}",
				   "Set-Cookie: br_session=; Path=/; Max-Age=0; HttpOnly\r\n");
		return;
	}

	browserremote_cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	if (strcmp(target, "/api/status") == 0) {
		cmd.command = BR_CMD_STATUS;
	} else if (strcmp(target, "/api/queue") == 0) {
		cmd.command = BR_CMD_QUEUE;
	} else if (strcmp(target, "/api/favourites") == 0) {
		cmd.command = BR_CMD_FAVOURITES;
	} else if (strcmp(target, "/api/library") == 0) {
		cmd.command = BR_CMD_LIBRARY;
		query_param(query, "q", cmd.query, sizeof(cmd.query));
		char offset[32];
		query_param(query, "offset", offset, sizeof(offset));
		cmd.index = atoi(offset);
	} else if (strcmp(target, "/api/control") == 0) {
		if (!command_from_control(request_body, &cmd)) {
			reply_json(fd, 400, "Bad Request", "{\"error\":\"bad_command\"}", NULL);
			return;
		}
	} else {
		reply_json(fd, 404, "Not Found", "{\"error\":\"not_found\"}", NULL);
		return;
	}
	do_command(fd, &cmd);
}

// ---------------------------------------------------------------------------
// Server thread and lifecycle
// ---------------------------------------------------------------------------

static void *server_thread(void *arg)
{
	(void)arg;
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		return NULL;
	}
	int reuse = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(BROWSERREMOTE_PORT);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 4) < 0) {
		close(listen_fd);
		return NULL;
	}

	while (!g_stop) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(listen_fd, &fds);
		struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
		int ready = select(listen_fd + 1, &fds, NULL, NULL, &tv);
		if (ready <= 0) {
			continue;
		}
		int client = accept(listen_fd, NULL, NULL);
		if (client < 0) {
			continue;
		}

		char *request = malloc(REQ_MAX + 1);
		if (request == NULL) {
			close(client);
			continue;
		}
		size_t used = 0;
		bool complete = false;
		while (used < REQ_MAX) {
			ssize_t n = recv(client, request + used, REQ_MAX - used, 0);
			if (n <= 0) {
				break;
			}
			used += (size_t)n;
			char *end = strstr(request, "\r\n\r\n");
			if (end != NULL) {
				char *len_header = strstr(request, "Content-Length:");
				size_t header_len = (size_t)(end + 4 - request);
				size_t need = header_len;
				if (len_header != NULL && len_header < end) {
					need += (size_t)atoi(skip_ws(len_header + strlen("Content-Length:")));
				}
				if (used >= need) {
					complete = true;
					break;
				}
			}
		}
		if (complete) {
			handle_request(client, request, used);
		}
		free(request);
		close(client);
	}
	close(listen_fd);
	return NULL;
}

void browserremote_http_init(void)
{
	memset(&g_ctx, 0, sizeof(g_ctx));
	browserremote_init(&g_ctx, &g_ops);

	const char *hash = config_get("remote", "pin_hash", "");
	browserremote_restore_pin_hash(&g_ctx, hash);
	g_enabled = config_get_bool("remote", "enabled", false);
	browserremote_touch(&g_ctx, time(NULL));
}

bool browserremote_http_has_pin(void)
{
	bool has;
	pthread_mutex_lock(&g_lock);
	has = browserremote_has_pin(&g_ctx);
	pthread_mutex_unlock(&g_lock);
	return has;
}

void browserremote_http_set_pin(const char *pin)
{
	pthread_mutex_lock(&g_lock);
	browserremote_set_pin(&g_ctx, pin);
	config_set("remote", "pin_hash", browserremote_pin_hash(&g_ctx));
	config_save();
	pthread_mutex_unlock(&g_lock);
}

bool browserremote_http_get_enabled(void) { return g_enabled; }

void browserremote_http_set_enabled(bool on)
{
	g_enabled = on;
	config_set_bool("remote", "enabled", on);
	config_save();
	if (on) {
		browserremote_http_start();
	} else {
		browserremote_http_stop();
	}
}

bool browserremote_http_running(void) { return g_running; }

bool browserremote_http_start(void)
{
	if (g_running) {
		browserremote_touch(&g_ctx, time(NULL));
		return true;
	}
	g_stop = false;
	if (pthread_create(&g_thread, NULL, server_thread, NULL) != 0) {
		return false;
	}
	g_thread_created = true;
	g_running = true;
	browserremote_touch(&g_ctx, time(NULL));
	return true;
}

void browserremote_http_stop(void)
{
	if (!g_running) {
		return;
	}
	g_stop = true;
	if (g_thread_created) {
		pthread_join(g_thread, NULL);
		g_thread_created = false;
	}
	g_running = false;
}

void browserremote_http_watchdog(void)
{
	bool idle = false;
	time_t now = time(NULL);
	pthread_mutex_lock(&g_lock);
	browserremote_expire(&g_ctx, now);
	if (g_running && browserremote_idle(&g_ctx, now) >= BROWSERREMOTE_IDLE_SECONDS) {
		idle = true;
	}
	pthread_mutex_unlock(&g_lock);
	if (idle) {
		browserremote_http_stop();
	}
}

void browserremote_http_url(char *out, int size)
{
	char address[128];
	if (!wifi_interface_address(address, sizeof(address)) || address[0] == '\0') {
		out[0] = '\0';
		return;
	}
	snprintf(out, (size_t)size, "http://%s:%d", address, BROWSERREMOTE_PORT);
}
