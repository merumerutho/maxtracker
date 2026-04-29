/*
 * test.h — Minimal unit test framework for maxtracker (NDS ARM9).
 */

#ifndef MT_TEST_H
#define MT_TEST_H

#include <nds.h>
#include <stdio.h>

/* Test result tracking */
typedef struct {
    int passed;
    int failed;
    int total;
} MT_TestResults;

extern MT_TestResults test_results;

/* Macros */
#define MT_ASSERT(cond, msg) do { \
    test_results.total++; \
    if (!(cond)) { \
        iprintf("FAIL: %s:%d %s\n", __FILE__, __LINE__, msg); \
        test_results.failed++; \
    } else { \
        test_results.passed++; \
    } \
} while(0)

#define MT_ASSERT_EQ(a, b, msg) MT_ASSERT((a) == (b), msg)

/* Run all tests, print summary. Returns 0 if all pass. */
int mt_run_tests(void);

#endif /* MT_TEST_H */
