/*
 * Browser remote core. See browserremote.h for the contract.
 *
 * Stub with the full interface in place: the tests can compile and run against
 * this, and fail on every assertion, before any behaviour is implemented.
 */

#include "browserremote.h"

#include <string.h>

void browserremote_init(browserremote_ctx_t *ctx, const browserremote_ops_t *ops)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->ops = ops;
}

bool browserremote_has_pin(const browserremote_ctx_t *ctx)
{
	(void)ctx;
	return false;
}

void browserremote_set_pin(browserremote_ctx_t *ctx, const char *pin)
{
	(void)ctx;
	(void)pin;
}

bool browserremote_verify_pin(const browserremote_ctx_t *ctx, const char *pin)
{
	(void)ctx;
	(void)pin;
	return false;
}

const char *browserremote_pin_hash(const browserremote_ctx_t *ctx)
{
	(void)ctx;
	return "";
}

void browserremote_restore_pin_hash(browserremote_ctx_t *ctx, const char *hash)
{
	(void)ctx;
	(void)hash;
}

const char *browserremote_challenge(browserremote_ctx_t *ctx, const char *browser_id, time_t now)
{
	(void)ctx;
	(void)browser_id;
	(void)now;
	return "";
}

bool browserremote_login(browserremote_ctx_t *ctx, const char *browser_id, const char *nonce, const char *pin,
						 char *token_out, size_t token_size, time_t now)
{
	(void)ctx;
	(void)browser_id;
	(void)nonce;
	(void)pin;
	(void)token_out;
	(void)token_size;
	(void)now;
	return false;
}

bool browserremote_session_valid(const browserremote_ctx_t *ctx, const char *browser_id, const char *token,
								 time_t now)
{
	(void)ctx;
	(void)browser_id;
	(void)token;
	(void)now;
	return false;
}

void browserremote_logout(browserremote_ctx_t *ctx, const char *token)
{
	(void)ctx;
	(void)token;
}

void browserremote_expire(browserremote_ctx_t *ctx, time_t now)
{
	(void)ctx;
	(void)now;
}

bool browserremote_execute(browserremote_ctx_t *ctx, const browserremote_cmd_t *cmd, char *out, size_t out_size)
{
	(void)ctx;
	(void)cmd;
	(void)out;
	(void)out_size;
	return false;
}

void browserremote_touch(browserremote_ctx_t *ctx, time_t now)
{
	(void)ctx;
	(void)now;
}

time_t browserremote_idle(const browserremote_ctx_t *ctx, time_t now)
{
	(void)ctx;
	(void)now;
	return 0;
}
