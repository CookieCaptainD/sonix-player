#include "listsort.h"

#include <stdlib.h>
#include <string.h>

// qsort takes no context, and the keys belong to the caller's list. The UI
// sorts on one thread, so one static pointer is enough.
static char (*sort_keys)[LIBRARY_SORT_KEY_MAX];

static int key_order(const void *a, const void *b) {
	int x = *(const int *)a;
	int y = *(const int *)b;
	int order = strcmp(sort_keys[x], sort_keys[y]);
	return order ? order : x - y;
}

void listsort_order(int *order, int count, char (*keys)[LIBRARY_SORT_KEY_MAX], listsort_mode_t mode) {
	if (count <= 0 || mode == LISTSORT_SOURCE) {
		return;
	}

	sort_keys = keys;
	qsort(order, (size_t)count, sizeof(*order), key_order);
	sort_keys = NULL;

	if (mode == LISTSORT_ZA) {
		for (int i = 0; i < count / 2; i++) {
			int swap = order[i];
			order[i] = order[count - 1 - i];
			order[count - 1 - i] = swap;
		}
	}
}

const char *listsort_label(listsort_mode_t mode) {
	static const char *const LABELS[] = {
		[LISTSORT_SOURCE] = "stream_sort_source",
		[LISTSORT_AZ] = "medialist_sort_name_az",
		[LISTSORT_ZA] = "medialist_sort_name_za",
	};

	if (mode < LISTSORT_SOURCE || mode > LISTSORT_ZA) {
		return LABELS[LISTSORT_SOURCE];
	}
	return LABELS[mode];
}
