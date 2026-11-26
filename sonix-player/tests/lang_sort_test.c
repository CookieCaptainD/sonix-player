// Host-only check: lang_list() must return language names in alphabetical
// order, whatever order readdir() hands back.
#include "src/system/core/lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	char dir[] = "/tmp/lang-sort-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 2;
	}

	// Mixed case: raw directory order puts capitals first, folded order puts
	// alpha, Beta, Čeština together. The two orders cannot coincide.
	const char *created[] = {"Zulu",	 "alpha",   "Ukrainian", "Russian", "Português BR",
							 "Polski",	 "Nederlands", "Japanese", "Italiano", "Français",
							 "Español", "Espanol", "English",	"Deutsch",	"Beta",
							 "Čeština"};
	const size_t total = sizeof(created) / sizeof(created[0]);
	for (size_t i = 0; i < total; i++) {
		write_ini(dir, created[i]);
	}

	lang_init(dir);
	const char *names[MAX_LANGUAGES] = {0};
	int count = lang_list(names);
	int status = 0;
	if (count != (int)total) {
		fprintf(stderr, "FAIL: expected %zu languages, got %d\n", total, count);
		status = 1;
	} else {
		const char *expected[] = {"alpha",	 "Beta",	  "Čeština",	"Deutsch", "English",
								  "Espanol", "Español",  "Français", "Italiano", "Japanese",
								  "Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
								  "Zulu"};
		for (int i = 0; i < count; i++) {
			if (strcmp(names[i], expected[i]) != 0) {
				fprintf(stderr, "FAIL: position %d: expected %s, got %s\n", i, expected[i], names[i]);
				status = 1;
			}
		}
	}
	if (!status) {
		puts("PASS: lang_list() returns names in alphabetical order");
	}

	for (size_t i = 0; i < total; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.ini", dir, created[i]);
		unlink(path);
	}
	rmdir(dir);
	return status;
}
