#ifndef LISTSORT_H
#define LISTSORT_H

#include "src/system/library/library.h"

// The orders the local lists offer. SOURCE keeps the order the page loaded,
// AZ and ZA sort by the library's own collation keys.
typedef enum {
	LISTSORT_SOURCE = 0,
	LISTSORT_AZ = 1,
	LISTSORT_ZA = 2,
} listsort_mode_t;

// Fills order[0..count-1] with source indices in display order. keys holds one
// library_sort_key() result per source index. SOURCE leaves order untouched.
void listsort_order(int *order, int count, char (*keys)[LIBRARY_SORT_KEY_MAX], listsort_mode_t mode);

// Language tag for a mode. The caller passes it through tr().
const char *listsort_label(listsort_mode_t mode);

#endif /* LISTSORT_H */
