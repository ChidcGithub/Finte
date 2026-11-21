#ifndef FINTE_SELFTEST_H
#define FINTE_SELFTEST_H

#include "finte/base.h"

typedef struct fselftest_report {
    uint32_t sections;
    uint32_t checks;
    uint32_t failures;
    char failed_section[FINTE_NAME_MAX];
} fselftest_report_t;

/* Runs a fast, self-contained core check against a clean core.
 *
 * The suite resets the core before each section and at the end. Call it after
 * hal_init()/finte_init() and before creating the boot scenario.
 *
 * verbose: 0 = silent, 1 = section summary, 2 = also print failed checks.
 * Returns 0 if every check passed, -1 otherwise.
 */
int finte_selftest_run(int verbose, fselftest_report_t *out);

#endif /* FINTE_SELFTEST_H */
