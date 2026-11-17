// The cover fetch client, on the host, against stubbed HTTP and the real JSON
// parser. Built and run by `make test-coverfetch`. Not part of host or target.

#include "src/system/library/coverfetch.h"
#include "src/system/library/coverfetch_test.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The HTTP layer, replaced here so no request leaves the process. The encoder
// mirrors the production one in http.c: every non-alphanumeric byte (spaces
// included) becomes %XX, so the query assertions test the real assembly.
static char stub_body[65536];
static char stub_url[2048];
static int stub_status = 200;
static int stub_calls = 0;
static char stub_last_error[256];

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out,
				 size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;
	stub_calls++;
	snprintf(stub_url, sizeof(stub_url), "%s", url ? url : "");
	*status_out = stub_status;
	if (stub_status < 200 || stub_status >= 300) {
		*out = NULL;
		return false;
	}
	size_t len = strlen(stub_body);
	*out = malloc(len + 1);
	if (!*out) {
		return false;
	}
	memcpy(*out, stub_body, len + 1);
	if (out_len) {
		*out_len = len;
	}
	return true;
}

const char *http_last_error(void) { return stub_last_error; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;
	for (size_t i = 0; in && in[i] && o + 4 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];
		bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
					c == '_' || c == '.' || c == '~';
		if (safe) {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = HEX[c >> 4];
			out[o++] = HEX[c & 0x0f];
		}
	}
	if (out_size) {
		out[o < out_size ? o : out_size - 1] = '\0';
	}
	return out;
}

// The hooks that set those stubs up. Implemented here beside the state they
// drive; coverfetch_test.h declares them.
void coverfetch_test_reset(void) {
	stub_body[0] = '\0';
	stub_url[0] = '\0';
	stub_status = 200;
	stub_calls = 0;
	stub_last_error[0] = '\0';
	coverfetch_negative_clear();
}

void coverfetch_test_set_body(const char *body) { snprintf(stub_body, sizeof(stub_body), "%s", body ? body : ""); }

void coverfetch_test_set_status(int status) { stub_status = status; }

int coverfetch_test_calls(void) { return stub_calls; }

const char *coverfetch_test_url(void) { return stub_url; }

#define CHECK(cond, name) \
	do { \
		if (cond) { \
			printf("ok   %s\n", name); \
		} else { \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
			exit(1); \
		} \
	} while (0)

// Two MusicBrainz releases, enough to check record building and freeing.
static const char *MB_BODY =
	"{\"releases\":["
	"{\"id\":\"rel-aaa\",\"title\":\"Abbey Road\"},"
	"{\"id\":\"rel-bbb\",\"title\":\"Let It Be\"}"
	"]}";

// 1. The built query encodes every space, and is what search sends.
static void case_query_encodes_album_and_artist(void) {
	char q[512];
	coverfetch_mb_query(q, sizeof(q), "Abbey Road", "The Beatles");
	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	coverfetch_free(records, count);
	const char *pinned = "query=release:Abbey%20Road%20AND%20artist:The%20Beatles";
	CHECK(strcmp(q, pinned) == 0 && strstr(coverfetch_test_url(), pinned) != NULL,
		  "query_encodes_album_and_artist");
}

// 2. A 404 is an answer: one request, not found, nothing handed back.
static void case_four_xx_not_retried(void) {
	coverfetch_test_reset();
	coverfetch_test_set_status(404);
	coverfetch_record_t *records = (coverfetch_record_t *)1;
	int count = -1;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NOT_FOUND && coverfetch_test_calls() == 1 && records == NULL && count == 0,
		  "four_xx_not_retried");
}

// 3. A 5xx is transient: three requests in all, then a network error.
static void case_five_xx_retries_thrice(void) {
	coverfetch_test_reset();
	coverfetch_test_set_status(500);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NETWORK && coverfetch_test_calls() == 3, "five_xx_retries_thrice");
}

// 4. With neither album nor artist there is nothing to ask for.
static void case_no_tags_short_circuits(void) {
	coverfetch_test_reset();
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("", "", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NO_TAGS && coverfetch_test_calls() == 0, "no_tags_short_circuits");
}

// 5. An album already marked not found is skipped without a request.
static void case_negative_cache_skips_request(void) {
	coverfetch_test_reset();
	char key[640];
	coverfetch_key(key, sizeof(key), "Abbey Road", "The Beatles");
	coverfetch_negative_add(key);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(coverfetch_negative_has(key) && r == COVERFETCH_NOT_FOUND && coverfetch_test_calls() == 0,
		  "negative_cache_skips_request");
}

// 6. A response that is not JSON is a network failure, not an empty result.
static void case_malformed_json_is_network(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body("{ this is not JSON");
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NETWORK && records == NULL && count == 0, "malformed_json_is_network");
}

// 7. No result path frees the array, and a successful one frees through the API.
static void case_search_frees_records(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body("{\"releases\":[{\"title\":\"no id\"},{\"title\":\"also no id\"}]}");
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	bool clean = r == COVERFETCH_NOT_FOUND && records == NULL && count == 0 &&
				 coverfetch_test_outstanding_records() == 0;

	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	clean = clean && r == COVERFETCH_OK && count == 2 && records != NULL &&
			strstr(records[1].url, "/rel-bbb/front-500") != NULL && records[0].size == 500;
	coverfetch_free(records, count);
	CHECK(clean && coverfetch_test_outstanding_records() == 0, "search_frees_records");
}

int main(void) {
	case_query_encodes_album_and_artist();
	case_four_xx_not_retried();
	case_five_xx_retries_thrice();
	case_no_tags_short_circuits();
	case_negative_cache_skips_request();
	case_malformed_json_is_network();
	case_search_frees_records();
	printf("all coverfetch cases passed\n");
	return 0;
}
