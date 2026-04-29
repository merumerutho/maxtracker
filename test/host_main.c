/*
 * host_main.c — Host-native test runner for maxtracker.
 *
 * Compiled with the system's gcc/clang (NOT devkitARM).
 * Links only pure-logic modules — no NDS hardware dependencies.
 *
 * Usage: make host-test
 */

#include "test.h"

int main(void)
{
    printf("=== maxtracker host-native unit tests ===\n\n");
    int result = mt_run_tests();
    return result;
}
