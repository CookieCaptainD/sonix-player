#ifndef BROWSERREMOTE_H
#define BROWSERREMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Browser remote core: PIN, browser-bound sessions and command dispatch.
//
// This module knows nothing about sockets, LVGL or the player. The HTTP layer
// parses requests into a browserremote_cmd_t and asks browserremote_execute()
// to run them; all access to the player goes through the injected ops table.
// Keeping the transport and the player out of here is what makes the two
// promises this feature makes testable in isolation:
//
//   * a control request is refused without a session bound to that browser;
//   * reading state, the queue or the library never touches playback.

#define BR_PIN_MAX 8
#define BR_ID_LEN 32
#define BR_TOKEN_LEN 32
#define BR_NONCE_LEN 32
#define BR_MAX_SESSIONS 8
#define BR_MAX_CHALLENGES 8
#define BR_SESSION_TTL 1800
#define BR_CHALLENGE_TTL 120

#define BR_PATH_LEN 512
#define BR_TITLE_LEN 256
#define BR_QUERY_LEN 128

// Mirrors audio_status_t without pulling the audio header into this module.
typedef enum {
	BR_STATUS_STOPPED = 0,
	BR_STATUS_PLAYING = 1,
	BR_STATUS_PAUSED = 2,
} browserremote_status_t;

typedef enum {
	BR_MODE_NORMAL = 0,
	BR_MODE_REPEAT_ALL = 1,
	BR_MODE_REPEAT_ONE = 2,
	BR_MODE_SHUFFLE = 3,
	BR_MODE_SHUFFLE_REPEAT = 4,
} browserremote_mode_t;

typedef enum {
	BR_CMD_STATUS,
	BR_CMD_QUEUE,
	BR_CMD_LIBRARY,
	BR_CMD_FAVOURITES,
	BR_CMD_TOGGLE_FAV,
	BR_CMD_PLAY_PAUSE,
	BR_CMD_NEXT,
	BR_CMD_PREV,
	BR_CMD_SEEK,
	BR_CMD_VOLUME,
	BR_CMD_MODE,
	BR_CMD_PLAY_PATH,
	BR_CMD_PLAY_INDEX,
} browserremote_command_t;

typedef struct {
	char path[BR_PATH_LEN];
	char title[BR_TITLE_LEN];
	char artist[BR_TITLE_LEN];
} browserremote_item_t;

typedef struct {
	int status;
	char path[BR_PATH_LEN];
	char title[BR_TITLE_LEN];
	char artist[BR_TITLE_LEN];
	double position;
	double duration;
	int volume;
	int mode;
	bool favourite;
	int queue_count;
	int queue_index;
} browserremote_state_t;

// Everything the core can ask of the player. Read commands may call only the
// first four; control commands call exactly one of the rest.
typedef struct {
	void (*state)(browserremote_state_t *out, void *user);
	int (*queue_count)(void *user);
	bool (*queue_item)(int index, browserremote_item_t *out, void *user);
	int (*library)(const char *query, int offset, int count, browserremote_item_t *out, int max, void *user);
	bool (*fav_contains)(const char *path, void *user);
	bool (*fav_toggle)(const char *path, const char *title, const char *artist, void *user);
	void (*play_pause)(void *user);
	void (*next)(void *user);
	void (*prev)(void *user);
	void (*seek)(double seconds, void *user);
	void (*volume)(int percent, void *user);
	void (*mode)(void *user);
	void (*play_path)(const char *path, void *user);
	void (*play_index)(int index, void *user);
	void *user;
} browserremote_ops_t;

typedef struct {
	char browser_id[BR_ID_LEN];
	char token[BR_TOKEN_LEN];
	time_t expires;
	bool used;
} br_session_t;

typedef struct {
	char browser_id[BR_ID_LEN];
	char nonce[BR_NONCE_LEN];
	time_t expires;
	bool used;
} br_challenge_t;

typedef struct {
	char pin_hash[32];
	const browserremote_ops_t *ops;
	br_session_t sessions[BR_MAX_SESSIONS];
	br_challenge_t challenges[BR_MAX_CHALLENGES];
	time_t last_activity;
} browserremote_ctx_t;

void browserremote_init(browserremote_ctx_t *ctx, const browserremote_ops_t *ops);

bool browserremote_has_pin(const browserremote_ctx_t *ctx);
void browserremote_set_pin(browserremote_ctx_t *ctx, const char *pin);
bool browserremote_verify_pin(const browserremote_ctx_t *ctx, const char *pin);
const char *browserremote_pin_hash(const browserremote_ctx_t *ctx);
void browserremote_restore_pin_hash(browserremote_ctx_t *ctx, const char *hash);

// A browser asks for a nonce once, then sends it back with the PIN. The nonce
// is single use and the session it creates carries the browser id: the token
// is useless from a different browser. The pointer returned by challenge()
// points into the context and stays valid until the next challenge; callers
// that need to keep it must copy it.
const char *browserremote_challenge(browserremote_ctx_t *ctx, const char *browser_id, time_t now);
bool browserremote_login(browserremote_ctx_t *ctx, const char *browser_id, const char *nonce, const char *pin,
						 char *token_out, size_t token_size, time_t now);
bool browserremote_session_valid(const browserremote_ctx_t *ctx, const char *browser_id, const char *token,
								 time_t now);
void browserremote_logout(browserremote_ctx_t *ctx, const char *token);
void browserremote_expire(browserremote_ctx_t *ctx, time_t now);

typedef struct {
	browserremote_command_t command;
	int index;
	double seconds;
	int percent;
	char path[BR_PATH_LEN];
	char title[BR_TITLE_LEN];
	char artist[BR_TITLE_LEN];
	char query[BR_QUERY_LEN];
} browserremote_cmd_t;

// Runs one already-authorised command. Writes a small JSON document into out.
// Returns false only when there is no ops table or the command is unknown.
bool browserremote_execute(browserremote_ctx_t *ctx, const browserremote_cmd_t *cmd, char *out, size_t out_size);

void browserremote_touch(browserremote_ctx_t *ctx, time_t now);
time_t browserremote_idle(const browserremote_ctx_t *ctx, time_t now);

#endif /* BROWSERREMOTE_H */
