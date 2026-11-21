/* Native polynomial CAS: exact rational coefficients in one variable. */
#ifndef FINTE_CAS_H
#define FINTE_CAS_H

#include "finte/base.h"

#define FINTE_CAS_MAX_DEGREE 32

typedef struct fcas_poly {
    int degree; /* -1 means zero */
    int64_t num[FINTE_CAS_MAX_DEGREE + 1];
    int64_t den[FINTE_CAS_MAX_DEGREE + 1]; /* positive, reduced */
} fcas_poly_t;

int fcas_parse(const char *expr, fcas_poly_t *out, char *err, size_t errcap);
int fcas_diff(const fcas_poly_t *in, fcas_poly_t *out);
int fcas_eval(const fcas_poly_t *p, int64_t xnum, int64_t xden,
              int64_t *onum, int64_t *oden);
int fcas_format(const fcas_poly_t *p, char *buf, size_t cap);

#endif /* FINTE_CAS_H */
