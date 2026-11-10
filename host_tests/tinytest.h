/*
 * Minimal self-contained test framework for the host tests -- no external
 * dependency (Unity etc.) since these tests just need to run with any plain
 * C99 compiler. A failed CHECK prints file:line and the failing expression
 * but lets the rest of the test function keep running, so one bad offset in
 * a status-buffer test doesn't hide the next ten.
 */
#pragma once

#include <stdio.h>

static int g_tt_checks;
static int g_tt_failures;

#define CHECK(cond) \
    do { \
        g_tt_checks++; \
        if (!(cond)) { \
            g_tt_failures++; \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        long long _tt_a = (long long)(a), _tt_b = (long long)(b); \
        g_tt_checks++; \
        if (_tt_a != _tt_b) { \
            g_tt_failures++; \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                    __FILE__, __LINE__, #a, #b, _tt_a, _tt_b); \
        } \
    } while (0)

#define RUN(fn) \
    do { \
        printf("-- %s --\n", #fn); \
        fn(); \
    } while (0)

static int tt_summary(void)
{
    printf("\n%d checks, %d failed\n", g_tt_checks, g_tt_failures);
    return g_tt_failures ? 1 : 0;
}
