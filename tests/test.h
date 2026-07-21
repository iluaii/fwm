#ifndef FWM_TEST_H
#define FWM_TEST_H

/* Just enough to write assertions and get a useful line out of a failure.
 * A framework would be more machinery than these suites are worth, and it
 * would be one more thing to install before the tests can run at all. */

#include <stdio.h>
#include <string.h>
#include <math.h>

static int t_checks = 0;
static int t_fails  = 0;
static const char *t_case = "";

/* Names the group a failure belongs to, so the message says what broke
 * rather than only where. */
#define CASE(name) do { t_case = (name); } while (0)

#define T_FAIL(fmt, ...) do { \
    t_fails++; \
    fprintf(stderr, "  FAIL [%s] %s:%d: " fmt "\n", \
            t_case, __FILE__, __LINE__, __VA_ARGS__); \
} while (0)

#define CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) T_FAIL("%s", #cond); \
} while (0)

#define CHECK_INT(got, want) do { \
    t_checks++; \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) T_FAIL("%s: got %lld, want %lld", #got, g_, w_); \
} while (0)

#define CHECK_DBL(got, want, eps) do { \
    t_checks++; \
    double g_ = (double)(got), w_ = (double)(want); \
    if (fabs(g_ - w_) > (eps)) \
        T_FAIL("%s: got %g, want %g (+-%g)", #got, g_, w_, (double)(eps)); \
} while (0)

#define CHECK_STR(got, want) do { \
    t_checks++; \
    const char *g_ = (got), *w_ = (want); \
    if (!g_ || !w_ || strcmp(g_, w_) != 0) \
        T_FAIL("%s: got \"%s\", want \"%s\"", #got, g_ ? g_ : "(null)", w_ ? w_ : "(null)"); \
} while (0)

#define CHECK_NULL(p)     do { t_checks++; if ((p) != NULL) T_FAIL("%s: expected NULL", #p); } while (0)
#define CHECK_NOT_NULL(p) do { t_checks++; if ((p) == NULL) T_FAIL("%s: unexpected NULL", #p); } while (0)

static int t_report(const char *suite) {
    if (t_fails) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", suite, t_fails, t_checks);
        return 1;
    }
    printf("%s: %d checks ok\n", suite, t_checks);
    return 0;
}

#endif /* FWM_TEST_H */
