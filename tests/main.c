/* SPDX-License-Identifier: MIT */
/*
 * Test runner. Every case prints [ok] or [FAIL]; the process exits non-zero
 * when any case failed.
 */
#include "tests.h"

int iosvm_test_verbose = 0;

static int g_total_cases = 0;
static int g_failed_cases = 0;
static int g_case_failed = 0;

void iosvm_test_fail(const char *file, int line, const char *expr)
{
    g_case_failed = 1;
    printf("    FAIL %s:%d: %s\n", file, line, expr);
}

int iosvm_test_eq_u64(const char *file, int line, const char *what, unsigned long long got,
                      unsigned long long want)
{
    if (got == want) {
        if (iosvm_test_verbose) {
            printf("    ok %s:%d %s (%llu)\n", file, line, what, got);
        }
        return 1;
    }
    g_case_failed = 1;
    printf("    FAIL %s:%d: %s: got %llu, want %llu\n", file, line, what, got, want);
    return 0;
}

int iosvm_test_mem_eq(const char *file, int line, const char *what, const void *got,
                      const void *want, size_t len)
{
    const uint8_t *a = (const uint8_t *)got;
    const uint8_t *b = (const uint8_t *)want;
    size_t i;

    for (i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            g_case_failed = 1;
            printf("    FAIL %s:%d: %s: byte %zu differs (got 0x%02x, want 0x%02x)\n", file, line,
                   what, i, (unsigned)a[i], (unsigned)b[i]);
            return 0;
        }
    }
    if (iosvm_test_verbose) {
        printf("    ok %s:%d %s (%zu bytes equal)\n", file, line, what, len);
    }
    return 1;
}

void iosvm_run_case(const char *name, void (*fn)(void))
{
    g_total_cases++;
    g_case_failed = 0;
    if (iosvm_test_verbose) {
        printf("  [ run ] %s\n", name);
    }
    fn();
    if (g_case_failed) {
        g_failed_cases++;
        printf("[FAIL] %s\n", name);
    } else {
        printf("[ok]   %s\n", name);
    }
}

static void usage(const char *argv0)
{
    printf("usage: %s [--verbose|-v] [--help|-h]\n", argv0);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            iosvm_test_verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "iosvm_tests: unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    printf("IOSVM GPU protocol v%u.%u test suite\n", (unsigned)IOSVM_PROTO_VERSION_MAJOR,
           (unsigned)IOSVM_PROTO_VERSION_MINOR);

    iosvm_test_proto();
    iosvm_test_ring();
    iosvm_test_codec();
    iosvm_test_fake_gpu();
    iosvm_test_integration();

    printf("\n%d/%d cases passed\n", g_total_cases - g_failed_cases, g_total_cases);
    return g_failed_cases == 0 ? 0 : 1;
}
