// Host-only check: lang_list() must return language names in alphabetical
// order, whatever order readdir() hands back.
#include "src/system/core/lang.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void write_ini(const char *dir, const char *name) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ini", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fprintf(f, "; %s\n", name);
	fclose(f);
}

int main(void) {
	const char *dir = "/tmp/opencode/lang-sort-test";
	if (mkdir("/tmp/opencode", 0777) != 0 && errno != EEXIST) {
		perror("mkdir /tmp/opencode");
		return 2;
	}
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		perror("mkdir test dir");
		return 2;
	}

	// Mixed case: raw directory order puts capitals first, folded order puts
	// alpha, Beta, Čeština together. The two orders cannot coincide.
	const char *created[] = {"Zulu",	 "alpha",   "Ukrainian", "Russian", "Português BR",
							 "Polski",	 "Nederlands", "Japanese", "Italiano", "Français",
							 "Español", "English", "Deutsch",	"Beta",		"Čeština"};
	const size_t total = sizeof(created) / sizeof(created[0]);
	for (size_t i = 0; i < total; i++) {
		write_ini(dir, created[i]);
	}

	lang_init(dir);
	const char *names[64] = {0};
	int count = lang_list(names);
	if (count != (int)total) {
		fprintf(stderr, "FAIL: expected %zu languages, got %d\n", total, count);
		return 1;
	}

	const char *expected[] = {"alpha",	 "Beta",	 "Čeština",	  "Deutsch",	"English",
							  "Español", "Français", "Italiano",  "Japanese", "Nederlands",
							  "Polski",	 "Português BR", "Russian", "Ukrainian", "Zulu"};
	int failed = 0;
	for (int i = 0; i < count; i++) {
		if (strcmp(names[i], expected[i]) != 0) {
			fprintf(stderr, "FAIL: position %d: expected %s, got %s\n", i, expected[i], names[i]);
			failed = 1;
		}
	}
	if (failed) {
		return 1;
	}
	puts("PASS: lang_list() returns names in alphabetical order");
	return 0;
}
