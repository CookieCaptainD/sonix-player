// The shared index ordering, on the host, against a small fixed key set.
// Built and run by `make test-listsort`. Not part of host or target.

#include "src/gui/shell/listsort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, name) \
	do { \
		if (cond) { \
			printf("ok   %s\n", name); \
		} else { \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
			exit(1); \
		} \
	} while (0)

static void fill(char (*keys)[LIBRARY_SORT_KEY_MAX], int count) {
	static const char *const NAMES[] = {"zeta", "Alpha", "beta", "alpha", "", "omega"};
	for (int i = 0; i < count; i++) {
		snprintf(keys[i], LIBRARY_SORT_KEY_MAX, "%s", NAMES[i]);
	}
}

static void identity(int *order, int count) {
	for (int i = 0; i < count; i++) {
		order[i] = i;
	}
}

static void case_source_keeps_order(void) {
	char keys[6][LIBRARY_SORT_KEY_MAX];
	int order[6];
	fill(keys, 6);
	identity(order, 6);
	listsort_order(order, 6, keys, LISTSORT_SOURCE);
	bool same = true;
	for (int i = 0; i < 6; i++) {
		same = same && order[i] == i;
	}
	CHECK(same, "source_keeps_order");
}

static void case_az(void) {
	char keys[6][LIBRARY_SORT_KEY_MAX];
	int order[6];
	fill(keys, 6);
	identity(order, 6);
	listsort_order(order, 6, keys, LISTSORT_AZ);
	// The empty key sorts first, the two alpha keys keep their source order,
	// then beta, omega and zeta.
	const int want[] = {4, 1, 3, 2, 5, 0};
	bool same = true;
	for (int i = 0; i < 6; i++) {
		same = same && order[i] == want[i];
	}
	CHECK(same, "name_az");
}

static void case_za(void) {
	char keys[6][LIBRARY_SORT_KEY_MAX];
	int az[6];
	int za[6];
	fill(keys, 6);
	identity(az, 6);
	listsort_order(az, 6, keys, LISTSORT_AZ);
	identity(za, 6);
	listsort_order(za, 6, keys, LISTSORT_ZA);
	bool reversed = true;
	for (int i = 0; i < 6; i++) {
		reversed = reversed && za[i] == az[5 - i];
	}
	CHECK(reversed, "name_za_reverse");
}

static void case_small_counts(void) {
	char keys[6][LIBRARY_SORT_KEY_MAX];
	int order[6];
	fill(keys, 6);
	identity(order, 6);
	listsort_order(order, 0, keys, LISTSORT_AZ);
	listsort_order(order, 1, keys, LISTSORT_ZA);
	CHECK(order[0] == 0 && order[1] == 1, "small_counts_untouched");
}

static void case_labels(void) {
	CHECK(strcmp(listsort_label(LISTSORT_SOURCE), "stream_sort_source") == 0, "label_source");
	CHECK(strcmp(listsort_label(LISTSORT_AZ), "medialist_sort_name_az") == 0, "label_az");
	CHECK(strcmp(listsort_label(LISTSORT_ZA), "medialist_sort_name_za") == 0, "label_za");
}

int main(void) {
	case_source_keeps_order();
	case_az();
	case_za();
	case_small_counts();
	case_labels();
	return 0;
}
