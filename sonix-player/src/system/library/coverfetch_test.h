#ifndef COVERFETCH_TEST_H
#define COVERFETCH_TEST_H

// Test-only hooks, compiled with COVERFETCH_TEST beside HOST_BUILD. No product
// build defines COVERFETCH_TEST.
//
// The stub controls are implemented in tools/test_coverfetch.c, beside the
// HTTP state they drive; the record counter lives in coverfetch.c under
// COVERFETCH_TEST so a "no result" path can be checked for leaks.

void coverfetch_test_reset(void);
void coverfetch_test_set_body(const char *body);
void coverfetch_test_set_status(int status);
int coverfetch_test_calls(void);
const char *coverfetch_test_url(void);

int coverfetch_test_outstanding_records(void);

#endif /* COVERFETCH_TEST_H */
