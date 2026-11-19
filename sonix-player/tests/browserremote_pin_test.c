/*
 * PIN challenge and browser-bound session handling for the browser remote
 * (src/system/net/browserremote.c).
 *
 * The promise under test: a PIN set on the player gates every control request,
 * and the session that a correct PIN opens belongs to one browser. A token
 * copied into another browser, a replayed nonce and an expired session are all
 * refused.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "system/net/browserremote.h"

static int failures;

static void check(int condition, const char *what)
{
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void check_str(const char *got, const char *want, const char *what)
{
	if (!got || strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\", wanted \"%s\"\n", what, got ? got : "(null)", want ? want : "(null)");
		failures++;
	}
}

/* No player access is needed to log in, so the ops table stays empty. */
static const browserremote_ops_t no_ops = {0};

static void test_pin_storage(void)
{
	browserremote_ctx_t ctx;
	browserremote_init(&ctx, &no_ops);

	check(!browserremote_has_pin(&ctx), "a fresh remote has no PIN");
	check(!browserremote_verify_pin(&ctx, "1234"), "a fresh remote refuses every PIN");

	browserremote_set_pin(&ctx, "1234");
	check(browserremote_has_pin(&ctx), "setting a PIN makes it required");
	check(browserremote_verify_pin(&ctx, "1234"), "the set PIN is accepted");
	check(!browserremote_verify_pin(&ctx, "0000"), "a different PIN is refused");
	check(!browserremote_verify_pin(&ctx, "12345"), "a longer PIN is refused");

	/* The hash is what gets persisted in device_config.ini and read back. */
	const char *hash = browserremote_pin_hash(&ctx);
	check(hash && hash[0], "the PIN has a stored hash");
	check(strcmp(hash, "1234") != 0, "the stored hash is not the plain PIN");

	browserremote_ctx_t restored;
	browserremote_init(&restored, &no_ops);
	browserremote_restore_pin_hash(&restored, hash);
	check(browserremote_verify_pin(&restored, "1234"), "a restored hash still verifies the PIN");
	check(!browserremote_verify_pin(&restored, "0000"), "a restored hash still refuses a wrong PIN");
}

static void test_challenge_and_login(void)
{
	browserremote_ctx_t ctx;
	browserremote_init(&ctx, &no_ops);
	browserremote_set_pin(&ctx, "2468");

	time_t now = 1000;
	const char *nonce = browserremote_challenge(&ctx, "browser-A", now);
	check(nonce && nonce[0], "a challenge returns a nonce");
	check_str(nonce, nonce, "nonce is stable for the caller");

	char token[BR_TOKEN_LEN];
	check(!browserremote_login(&ctx, "browser-A", "wrong-nonce", "2468", token, sizeof(token), now),
		  "login with an unknown nonce is refused");
	check(!browserremote_login(&ctx, "browser-A", nonce, "0000", token, sizeof(token), now),
		  "login with a wrong PIN is refused");

	/* The failed attempt above must not consume the nonce: typing a PIN wrong
	 * once has to be recoverable from the same browser page. */
	check(browserremote_login(&ctx, "browser-A", nonce, "2468", token, sizeof(token), now),
		  "login with the right nonce and PIN succeeds");
	check(token[0], "a successful login returns a token");
	check(browserremote_session_valid(&ctx, "browser-A", token, now), "the token is valid for its browser");
	check(!browserremote_session_valid(&ctx, "browser-B", token, now), "the token is refused from another browser");
	check(!browserremote_session_valid(&ctx, "browser-A", "not-a-token", now), "an unknown token is refused");

	check(!browserremote_login(&ctx, "browser-A", nonce, "2468", token, sizeof(token), now),
		  "a nonce cannot be reused");
}

static void test_rechallenge_and_logout(void)
{
	browserremote_ctx_t ctx;
	browserremote_init(&ctx, &no_ops);
	browserremote_set_pin(&ctx, "1111");

	time_t now = 2000;
	const char *first = browserremote_challenge(&ctx, "browser-A", now);
	const char *second = browserremote_challenge(&ctx, "browser-A", now + 1);
	check(second && second[0] && strcmp(first, second) != 0, "a new challenge issues a new nonce");

	char token[BR_TOKEN_LEN];
	check(!browserremote_login(&ctx, "browser-A", first, "1111", token, sizeof(token), now + 1),
		  "an outdated nonce is refused");
	check(browserremote_login(&ctx, "browser-A", second, "1111", token, sizeof(token), now + 1),
		  "the newest nonce still works");

	browserremote_logout(&ctx, token);
	check(!browserremote_session_valid(&ctx, "browser-A", token, now + 1), "logout invalidates the token");
}

static void test_expiry(void)
{
	browserremote_ctx_t ctx;
	browserremote_init(&ctx, &no_ops);
	browserremote_set_pin(&ctx, "1234");

	time_t now = 3000;
	const char *nonce = browserremote_challenge(&ctx, "browser-A", now);
	char token[BR_TOKEN_LEN];
	check(browserremote_login(&ctx, "browser-A", nonce, "1234", token, sizeof(token), now),
		  "login succeeds before the challenge expires");
	check(browserremote_session_valid(&ctx, "browser-A", token, now + BR_SESSION_TTL - 1),
		  "the session is valid just before its TTL");
	check(!browserremote_session_valid(&ctx, "browser-A", token, now + BR_SESSION_TTL),
		  "the session is refused at its TTL");

	const char *old = browserremote_challenge(&ctx, "browser-A", now);
	check(!browserremote_login(&ctx, "browser-A", old, "1234", token, sizeof(token), now + BR_CHALLENGE_TTL),
		  "an expired challenge is refused");
}

int main(void)
{
	test_pin_storage();
	test_challenge_and_login();
	test_rechallenge_and_logout();
	test_expiry();

	if (failures == 0) {
		printf("browserremote_pin_test: all checks passed\n");
		return 0;
	}
	printf("browserremote_pin_test: %d failure(s)\n", failures);
	return 1;
}
