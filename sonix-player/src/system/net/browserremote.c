/*
 * Browser remote core: PIN, browser-bound sessions and command dispatch.
 *
 * See browserremote.h for the contract. Nothing here opens a socket or talks
 * to the player directly: the HTTP layer hands in a command and an ops table,
 * and the two guarantees the feature makes are decided in this file.
 */

#include "browserremote.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LIBRARY_PAGE 50

// Seed for tokens and nonces. Random once per process, with a counter on top so
// two challenges in the same second still differ even without /dev/urandom.
static uint64_t id_seed;
static uint64_t id_seq;

static void seed_ids(void)
{
	if (id_seed != 0) {
		return;
	}
	FILE *f = fopen("/dev/urandom", "rb");
	if (f != NULL) {
		if (fread(&id_seed, sizeof(id_seed), 1, f) != 1) {
			id_seed = 0;
		}
		fclose(f);
	}
	if (id_seed == 0) {
		id_seed = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(uintptr_t)&id_seed;
	}
}

static uint64_t fnv1a(const char *s)
{
	uint64_t h = 1469598103934665603ULL;
	for (; s != NULL && *s != '\0'; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

static void make_id(char *out, size_t out_size, const char *scope, time_t now)
{
	seed_ids();
	uint64_t v = id_seed ^ (++id_seq * 0x9e3779b97f4a7c15ULL) ^ (uint64_t)now ^ fnv1a(scope);
	snprintf(out, out_size, "%016llx", (unsigned long long)v);
}

// ---------------------------------------------------------------------------
// Bounded JSON writing
// ---------------------------------------------------------------------------

typedef struct {
	char *buf;
	size_t size;
	size_t len;
} json_t;

static void jf(json_t *j, const char *fmt, ...)
{
	if (j->len >= j->size) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	size_t room = j->size - j->len;
	j->len += ((size_t)n < room) ? (size_t)n : room - 1;
}

static void jstring(json_t *j, const char *s)
{
	jf(j, "\"");
	if (s == NULL) {
		s = "";
	}
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"':
			jf(j, "\\\"");
			break;
		case '\\':
			jf(j, "\\\\");
			break;
		case '\n':
			jf(j, "\\n");
			break;
		case '\r':
			jf(j, "\\r");
			break;
		case '\t':
			jf(j, "\\t");
			break;
		default:
			if (c < 0x20) {
				jf(j, "\\u%04x", c);
			} else {
				jf(j, "%c", c);
			}
		}
	}
	jf(j, "\"");
}

static void jitem(json_t *j, const browserremote_item_t *it)
{
	jf(j, "{\"path\":");
	jstring(j, it->path);
	jf(j, ",\"title\":");
	jstring(j, it->title);
	jf(j, ",\"artist\":");
	jstring(j, it->artist);
	jf(j, "}");
}

// ---------------------------------------------------------------------------
// Lifecycle and PIN
// ---------------------------------------------------------------------------

void browserremote_init(browserremote_ctx_t *ctx, const browserremote_ops_t *ops)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->ops = ops;
}

bool browserremote_has_pin(const browserremote_ctx_t *ctx)
{
	return ctx->pin_hash[0] != '\0';
}

void browserremote_set_pin(browserremote_ctx_t *ctx, const char *pin)
{
	if (pin == NULL || pin[0] == '\0') {
		ctx->pin_hash[0] = '\0';
		return;
	}
	snprintf(ctx->pin_hash, sizeof(ctx->pin_hash), "%016llx", (unsigned long long)fnv1a(pin));
}

bool browserremote_verify_pin(const browserremote_ctx_t *ctx, const char *pin)
{
	if (!browserremote_has_pin(ctx) || pin == NULL || pin[0] == '\0') {
		return false;
	}
	char candidate[32];
	snprintf(candidate, sizeof(candidate), "%016llx", (unsigned long long)fnv1a(pin));
	return strcmp(candidate, ctx->pin_hash) == 0;
}

const char *browserremote_pin_hash(const browserremote_ctx_t *ctx)
{
	return ctx->pin_hash;
}

void browserremote_restore_pin_hash(browserremote_ctx_t *ctx, const char *hash)
{
	ctx->pin_hash[0] = '\0';
	if (hash != NULL) {
		snprintf(ctx->pin_hash, sizeof(ctx->pin_hash), "%s", hash);
	}
}

// ---------------------------------------------------------------------------
// Challenge and sessions
// ---------------------------------------------------------------------------

static br_challenge_t *challenge_slot(browserremote_ctx_t *ctx, time_t now)
{
	// Reuse an expired or consumed slot before taking a live one, so a burst of
	// failed logins cannot lock out the browser that is actually trying.
	for (int i = 0; i < BR_MAX_CHALLENGES; i++) {
		if (!ctx->challenges[i].used || ctx->challenges[i].expires <= now) {
			return &ctx->challenges[i];
		}
	}
	return &ctx->challenges[0];
}

const char *browserremote_challenge(browserremote_ctx_t *ctx, const char *browser_id, time_t now)
{
	if (browser_id == NULL || browser_id[0] == '\0') {
		return "";
	}
	// One live challenge per browser. Asking for a new nonce retires the old
	// one, so a nonce seen earlier cannot be replayed against a later login.
	for (int i = 0; i < BR_MAX_CHALLENGES; i++) {
		if (ctx->challenges[i].used && strcmp(ctx->challenges[i].browser_id, browser_id) == 0) {
			ctx->challenges[i].used = false;
		}
	}
	br_challenge_t *c = challenge_slot(ctx, now);
	memset(c, 0, sizeof(*c));
	snprintf(c->browser_id, sizeof(c->browser_id), "%s", browser_id);
	make_id(c->nonce, sizeof(c->nonce), browser_id, now);
	c->expires = now + BR_CHALLENGE_TTL;
	c->used = true;
	return c->nonce;
}

static br_session_t *session_slot(browserremote_ctx_t *ctx, time_t now)
{
	for (int i = 0; i < BR_MAX_SESSIONS; i++) {
		if (!ctx->sessions[i].used || ctx->sessions[i].expires <= now) {
			return &ctx->sessions[i];
		}
	}
	return &ctx->sessions[0];
}

bool browserremote_login(browserremote_ctx_t *ctx, const char *browser_id, const char *nonce, const char *pin,
						 char *token_out, size_t token_size, time_t now)
{
	if (token_out != NULL && token_size > 0) {
		token_out[0] = '\0';
	}
	if (browser_id == NULL || nonce == NULL || !browserremote_verify_pin(ctx, pin)) {
		return false;
	}

	br_challenge_t *c = NULL;
	for (int i = 0; i < BR_MAX_CHALLENGES; i++) {
		br_challenge_t *candidate = &ctx->challenges[i];
		if (candidate->used && candidate->expires > now && strcmp(candidate->browser_id, browser_id) == 0 &&
			strcmp(candidate->nonce, nonce) == 0) {
			c = candidate;
			break;
		}
	}
	if (c == NULL) {
		return false;
	}

	br_session_t *s = session_slot(ctx, now);
	memset(s, 0, sizeof(*s));
	snprintf(s->browser_id, sizeof(s->browser_id), "%s", browser_id);
	make_id(s->token, sizeof(s->token), browser_id, now);
	s->expires = now + BR_SESSION_TTL;
	s->used = true;

	// The nonce buys exactly one session.
	c->used = false;

	if (token_out != NULL && token_size > 0) {
		snprintf(token_out, token_size, "%s", s->token);
	}
	return true;
}

bool browserremote_session_valid(const browserremote_ctx_t *ctx, const char *browser_id, const char *token,
								 time_t now)
{
	if (browser_id == NULL || token == NULL || token[0] == '\0') {
		return false;
	}
	for (int i = 0; i < BR_MAX_SESSIONS; i++) {
		const br_session_t *s = &ctx->sessions[i];
		if (s->used && s->expires > now && strcmp(s->browser_id, browser_id) == 0 &&
			strcmp(s->token, token) == 0) {
			return true;
		}
	}
	return false;
}

void browserremote_logout(browserremote_ctx_t *ctx, const char *token)
{
	if (token == NULL) {
		return;
	}
	for (int i = 0; i < BR_MAX_SESSIONS; i++) {
		if (ctx->sessions[i].used && strcmp(ctx->sessions[i].token, token) == 0) {
			ctx->sessions[i].used = false;
		}
	}
}

void browserremote_expire(browserremote_ctx_t *ctx, time_t now)
{
	for (int i = 0; i < BR_MAX_CHALLENGES; i++) {
		if (ctx->challenges[i].used && ctx->challenges[i].expires <= now) {
			ctx->challenges[i].used = false;
		}
	}
	for (int i = 0; i < BR_MAX_SESSIONS; i++) {
		if (ctx->sessions[i].used && ctx->sessions[i].expires <= now) {
			ctx->sessions[i].used = false;
		}
	}
}

void browserremote_touch(browserremote_ctx_t *ctx, time_t now)
{
	ctx->last_activity = now;
}

time_t browserremote_idle(const browserremote_ctx_t *ctx, time_t now)
{
	if (ctx->last_activity == 0) {
		return 0;
	}
	return now - ctx->last_activity;
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static void do_status(browserremote_ctx_t *ctx, json_t *j)
{
	browserremote_state_t st;
	memset(&st, 0, sizeof(st));
	if (ctx->ops->state != NULL) {
		ctx->ops->state(&st, ctx->ops->user);
	}
	bool fav = (ctx->ops->fav_contains != NULL) ? ctx->ops->fav_contains(st.path, ctx->ops->user) : false;

	jf(j, "{\"status\":%d,\"path\":", st.status);
	jstring(j, st.path);
	jf(j, ",\"title\":");
	jstring(j, st.title);
	jf(j, ",\"artist\":");
	jstring(j, st.artist);
	jf(j, ",\"position\":%g,\"duration\":%g,\"volume\":%d,\"mode\":%d,\"favourite\":%s,"
	   "\"queue_count\":%d,\"queue_index\":%d}",
	   st.position, st.duration, st.volume, st.mode, fav ? "true" : "false", st.queue_count, st.queue_index);
}

static void do_queue(browserremote_ctx_t *ctx, json_t *j)
{
	int count = (ctx->ops->queue_count != NULL) ? ctx->ops->queue_count(ctx->ops->user) : 0;
	int index = -1;
	browserremote_state_t st;
	memset(&st, 0, sizeof(st));
	if (ctx->ops->state != NULL) {
		ctx->ops->state(&st, ctx->ops->user);
		index = st.queue_index;
	}

	jf(j, "{\"count\":%d,\"index\":%d,\"items\":[", count, index);
	for (int i = 0; i < count; i++) {
		browserremote_item_t it;
		memset(&it, 0, sizeof(it));
		if (ctx->ops->queue_item == NULL || !ctx->ops->queue_item(i, &it, ctx->ops->user)) {
			continue;
		}
		if (i > 0) {
			jf(j, ",");
		}
		jitem(j, &it);
	}
	jf(j, "]}");
}

static void do_library(browserremote_ctx_t *ctx, const browserremote_cmd_t *cmd, json_t *j, bool favourites)
{
	browserremote_item_t items[LIBRARY_PAGE];
	int count = 0;
	if (ctx->ops->library != NULL) {
		const char *query = favourites ? "@fav" : (cmd->query[0] ? cmd->query : "");
		count = ctx->ops->library(query, cmd->index, LIBRARY_PAGE, items, LIBRARY_PAGE, ctx->ops->user);
	}
	if (count < 0) {
		count = 0;
	}
	if (count > LIBRARY_PAGE) {
		count = LIBRARY_PAGE;
	}

	jf(j, "{\"items\":[");
	for (int i = 0; i < count; i++) {
		if (i > 0) {
			jf(j, ",");
		}
		jitem(j, &items[i]);
	}
	jf(j, "]}");
}

bool browserremote_execute(browserremote_ctx_t *ctx, const browserremote_cmd_t *cmd, char *out, size_t out_size)
{
	if (ctx == NULL || ctx->ops == NULL || cmd == NULL || out == NULL || out_size == 0) {
		return false;
	}

	json_t j = {out, out_size, 0};
	out[0] = '\0';

	switch (cmd->command) {
	case BR_CMD_STATUS:
		do_status(ctx, &j);
		break;
	case BR_CMD_QUEUE:
		do_queue(ctx, &j);
		break;
	case BR_CMD_LIBRARY:
		do_library(ctx, cmd, &j, false);
		break;
	case BR_CMD_FAVOURITES:
		do_library(ctx, cmd, &j, true);
		break;
	case BR_CMD_TOGGLE_FAV:
		if (ctx->ops->fav_toggle != NULL) {
			ctx->ops->fav_toggle(cmd->path, cmd->title, cmd->artist, ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_PLAY_PAUSE:
		if (ctx->ops->play_pause != NULL) {
			ctx->ops->play_pause(ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_NEXT:
		if (ctx->ops->next != NULL) {
			ctx->ops->next(ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_PREV:
		if (ctx->ops->prev != NULL) {
			ctx->ops->prev(ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_SEEK:
		if (ctx->ops->seek != NULL) {
			ctx->ops->seek(cmd->seconds, ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_VOLUME:
		if (ctx->ops->volume != NULL) {
			ctx->ops->volume(cmd->percent, ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_MODE:
		if (ctx->ops->mode != NULL) {
			ctx->ops->mode(ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_PLAY_PATH:
		if (ctx->ops->play_path != NULL) {
			ctx->ops->play_path(cmd->path, ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	case BR_CMD_PLAY_INDEX:
		if (ctx->ops->play_index != NULL) {
			ctx->ops->play_index(cmd->index, ctx->ops->user);
		}
		jf(&j, "{\"ok\":true}");
		break;
	default:
		return false;
	}

	return true;
}
