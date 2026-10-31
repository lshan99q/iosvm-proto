/* SPDX-License-Identifier: MIT */
/*
 * Tiny test harness for the IOSVM protocol library.
 *
 * No framework: each case is a void function that reports through CHECK() and
 * the runner prints [ok] or [FAIL] per case. A failed CHECK aborts the case (the
 * failure is still recorded) so that one bad assumption does not cascade.
 */
#ifndef IOSVM_TESTS_H
#define IOSVM_TESTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iosvm_codec.h"
#include "iosvm_fake_gpu.h"
#include "iosvm_gpu.h"
#include "iosvm_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set by --verbose; prints every passing check as well as each case name. */
extern int iosvm_test_verbose;

void iosvm_test_fail(const char *file, int line, const char *expr);
int iosvm_test_eq_u64(const char *file, int line, const char *what, unsigned long long got,
                      unsigned long long want);
int iosvm_test_mem_eq(const char *file, int line, const char *what, const void *got,
                      const void *want, size_t len);

/* Run one case: prints [ok]/[FAIL] and updates the global counters. */
void iosvm_run_case(const char *name, void (*fn)(void));

/* Suites, one per test file. */
void iosvm_test_proto(void);
void iosvm_test_ring(void);
void iosvm_test_codec(void);
void iosvm_test_fake_gpu(void);
void iosvm_test_integration(void);

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            iosvm_test_fail(__FILE__, __LINE__, #cond);                                            \
            return;                                                                                \
        }                                                                                          \
        if (iosvm_test_verbose) {                                                                  \
            printf("    ok %s:%d %s\n", __FILE__, __LINE__, #cond);                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(got, want)                                                                        \
    do {                                                                                           \
        if (!iosvm_test_eq_u64(__FILE__, __LINE__, #got " == " #want, (unsigned long long)(got),   \
                               (unsigned long long)(want))) {                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_MEM(got, want, len)                                                                  \
    do {                                                                                           \
        if (!iosvm_test_mem_eq(__FILE__, __LINE__, #got " == " #want, (got), (want),               \
                               (size_t)(len))) {                                                   \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* IOSVM_TESTS_H */
