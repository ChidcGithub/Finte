/* Native CAS: exact rational polynomials in one variable.
 *
 * Supported: integers, x, + - * / (constant divisors), ^ non-negative
 * integer exponents, parentheses. Results are simplified polynomials with
 * rational coefficients. This is deliberately a compact CAS core, not a
 * general transcendental algebra system. */
#include "finte/cas.h"

typedef struct cas_parser {
    const char *start;
    const char *p;
    char *err;
    size_t errcap;
    int failed;
    int depth;
} cas_parser_t;

#define FCAS_MAX_DEPTH 8

static uint64_t abs_u64(int64_t v)
{
    return v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

static int set_frac(fcas_poly_t *out, int i, int64_t n, int64_t d)
{
    if (d == 0) {
        return -1;
    }
    if (d < 0) {
        if (d == INT64_MIN || n == INT64_MIN) {
            return -1;
        }
        n = -n;
        d = -d;
    }
    if (n == 0) {
        out->num[i] = 0;
        out->den[i] = 1;
        return 0;
    }
    uint64_t g = gcd_u64(abs_u64(n), (uint64_t)d);
    out->num[i] = n / (int64_t)g;
    out->den[i] = d / (int64_t)g;
    return 0;
}

static int add_frac(int64_t n1, int64_t d1, int64_t n2, int64_t d2,
                    int64_t *on, int64_t *od)
{
    uint64_t g = gcd_u64((uint64_t)d1, (uint64_t)d2);
    int64_t a = d1 / (int64_t)g;
    int64_t b = d2 / (int64_t)g;
    int64_t x;
    int64_t y;
    int64_t n;
    int64_t d;
    if (__builtin_mul_overflow(n1, b, &x) ||
        __builtin_mul_overflow(n2, a, &y) ||
        __builtin_add_overflow(x, y, &n) ||
        __builtin_mul_overflow(a, d2, &d)) {
        return -1;
    }
    *on = n;
    *od = d;
    return 0;
}

static int mul_frac(int64_t n1, int64_t d1, int64_t n2, int64_t d2,
                    int64_t *on, int64_t *od)
{
    uint64_t g1 = gcd_u64(abs_u64(n1), (uint64_t)d2);
    uint64_t g2 = gcd_u64(abs_u64(n2), (uint64_t)d1);
    int64_t a = n1 / (int64_t)g1;
    int64_t b = d2 / (int64_t)g1;
    int64_t c = n2 / (int64_t)g2;
    int64_t d = d1 / (int64_t)g2;
    if (__builtin_mul_overflow(a, c, on) ||
        __builtin_mul_overflow(b, d, od)) {
        return -1;
    }
    if (*od < 0) {
        if (*on == INT64_MIN) {
            return -1;
        }
        *on = -*on;
        *od = -*od;
    }
    return 0;
}

static void poly_zero(fcas_poly_t *p)
{
    finte_memset(p, 0, sizeof(*p));
    p->degree = -1;
    for (int i = 0; i <= FINTE_CAS_MAX_DEGREE; i++) {
        p->den[i] = 1;
    }
}

static int poly_normalize(fcas_poly_t *p)
{
    for (int i = 0; i <= FINTE_CAS_MAX_DEGREE; i++) {
        if (p->den[i] == 0) {
            p->den[i] = 1;
        }
        if (p->num[i] == 0) {
            p->den[i] = 1;
        } else if (p->den[i] < 0) {
            if (p->num[i] == INT64_MIN) {
                return -1;
            }
            p->num[i] = -p->num[i];
            p->den[i] = -p->den[i];
        }
    }
    int d = -1;
    for (int i = FINTE_CAS_MAX_DEGREE; i >= 0; i--) {
        if (p->num[i] != 0) {
            d = i;
            break;
        }
    }
    p->degree = d;
    return 0;
}

static int poly_const(fcas_poly_t *p, int64_t n, int64_t d)
{
    poly_zero(p);
    if (n == 0) {
        return 0;
    }
    if (set_frac(p, 0, n, d) != 0) {
        return -1;
    }
    p->degree = 0;
    return 0;
}

static int poly_var(fcas_poly_t *p)
{
    poly_zero(p);
    p->num[1] = 1;
    p->den[1] = 1;
    p->degree = 1;
    return 0;
}

static int poly_neg(fcas_poly_t *p)
{
    if (p->degree < 0) {
        return 0;
    }
    for (int i = 0; i <= p->degree; i++) {
        if (p->num[i] == INT64_MIN) {
            return -1;
        }
        p->num[i] = -p->num[i];
    }
    return 0;
}

static int poly_add(const fcas_poly_t *a, const fcas_poly_t *b,
                    fcas_poly_t *out, int subtract)
{
    fcas_poly_t r;
    poly_zero(&r);
    int max = a->degree > b->degree ? a->degree : b->degree;
    for (int i = 0; i <= max; i++) {
        int64_t n1 = i <= a->degree ? a->num[i] : 0;
        int64_t d1 = i <= a->degree ? a->den[i] : 1;
        int64_t n2 = i <= b->degree ? b->num[i] : 0;
        int64_t d2 = i <= b->degree ? b->den[i] : 1;
        if (subtract) {
            if (n2 == INT64_MIN) {
                return -1;
            }
            n2 = -n2;
        }
        int64_t n;
        int64_t d;
        if (add_frac(n1, d1, n2, d2, &n, &d) != 0 ||
            set_frac(&r, i, n, d) != 0) {
            return -1;
        }
    }
    if (poly_normalize(&r) != 0) {
        return -1;
    }
    *out = r;
    return 0;
}

static int poly_mul(const fcas_poly_t *a, const fcas_poly_t *b,
                    fcas_poly_t *out)
{
    fcas_poly_t r;
    poly_zero(&r);
    if (a->degree < 0 || b->degree < 0) {
        *out = r;
        return 0;
    }
    if (a->degree + b->degree > FINTE_CAS_MAX_DEGREE) {
        return -1;
    }
    for (int i = 0; i <= a->degree; i++) {
        if (!a->num[i]) {
            continue;
        }
        for (int j = 0; j <= b->degree; j++) {
            if (!b->num[j]) {
                continue;
            }
            int64_t n;
            int64_t d;
            int64_t sum_n;
            int64_t sum_d;
            if (mul_frac(a->num[i], a->den[i], b->num[j], b->den[j],
                         &n, &d) != 0 ||
                add_frac(r.num[i + j], r.den[i + j], n, d,
                         &sum_n, &sum_d) != 0 ||
                set_frac(&r, i + j, sum_n, sum_d) != 0) {
                return -1;
            }
        }
    }
    if (poly_normalize(&r) != 0) {
        return -1;
    }
    *out = r;
    return 0;
}

static int poly_pow(const fcas_poly_t *base, int exp, fcas_poly_t *out)
{
    if (exp < 0) {
        return -1;
    }
    fcas_poly_t result;
    poly_const(&result, 1, 1);
    fcas_poly_t cur = *base;
    while (exp > 0) {
        if (exp & 1) {
            fcas_poly_t next;
            if (poly_mul(&result, &cur, &next) != 0) {
                return -1;
            }
            result = next;
        }
        exp >>= 1;
        if (exp) {
            fcas_poly_t next;
            if (poly_mul(&cur, &cur, &next) != 0) {
                return -1;
            }
            cur = next;
        }
    }
    *out = result;
    return 0;
}

static int poly_div_const(const fcas_poly_t *a, const fcas_poly_t *b,
                          fcas_poly_t *out)
{
    if (b->degree != 0 || b->num[0] == 0) {
        return -1;
    }
    fcas_poly_t r;
    poly_zero(&r);
    for (int i = 0; i <= a->degree; i++) {
        int64_t n;
        int64_t d;
        if (mul_frac(a->num[i], a->den[i], b->den[0], b->num[0],
                     &n, &d) != 0 ||
            set_frac(&r, i, n, d) != 0) {
            return -1;
        }
    }
    if (poly_normalize(&r) != 0) {
        return -1;
    }
    *out = r;
    return 0;
}

static void skip_ws(cas_parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
           *ps->p == '\r') {
        ps->p++;
    }
}

static void set_err(cas_parser_t *ps, const char *msg)
{
    if (ps->failed) {
        return;
    }
    ps->failed = 1;
    if (ps->err && ps->errcap) {
        finte_format(ps->err, ps->errcap, "%s at column %u", msg,
                     (unsigned)(ps->p - ps->start + 1));
    }
}

static int parse_expr(cas_parser_t *ps, fcas_poly_t *out);

static int parse_primary(cas_parser_t *ps, fcas_poly_t *out)
{
    skip_ws(ps);
    if (*ps->p == '(') {
        if (ps->depth >= FCAS_MAX_DEPTH) {
            set_err(ps, "expression nesting too deep");
            return -1;
        }
        ps->depth++;
        ps->p++;
        if (parse_expr(ps, out) != 0) {
            ps->depth--;
            return -1;
        }
        skip_ws(ps);
        if (*ps->p != ')') {
            ps->depth--;
            set_err(ps, "expected ')'");
            return -1;
        }
        ps->p++;
        ps->depth--;
        return 0;
    }
    if (*ps->p == 'x' || *ps->p == 'X') {
        ps->p++;
        if (poly_var(out) != 0) {
            set_err(ps, "degree limit exceeded");
            return -1;
        }
        return 0;
    }
    if (*ps->p >= '0' && *ps->p <= '9') {
        int64_t value = 0;
        while (*ps->p >= '0' && *ps->p <= '9') {
            int digit = *ps->p - '0';
            if (value > (INT64_MAX - digit) / 10) {
                set_err(ps, "integer overflow");
                return -1;
            }
            value = value * 10 + digit;
            ps->p++;
        }
        if (poly_const(out, value, 1) != 0) {
            set_err(ps, "invalid constant");
            return -1;
        }
        return 0;
    }
    set_err(ps, "expected a number, x, or '('");
    return -1;
}

static int parse_power(cas_parser_t *ps, fcas_poly_t *out)
{
    if (parse_primary(ps, out) != 0) {
        return -1;
    }
    skip_ws(ps);
    if (*ps->p != '^') {
        return 0;
    }
    ps->p++;
    skip_ws(ps);
    if (*ps->p < '0' || *ps->p > '9') {
        set_err(ps, "exponent must be a non-negative integer");
        return -1;
    }
    int exp = 0;
    while (*ps->p >= '0' && *ps->p <= '9') {
        int digit = *ps->p - '0';
        if (exp > (FINTE_CAS_MAX_DEGREE - digit) / 10) {
            set_err(ps, "exponent exceeds degree limit");
            return -1;
        }
        exp = exp * 10 + digit;
        ps->p++;
    }
    fcas_poly_t result;
    if (poly_pow(out, exp, &result) != 0) {
        set_err(ps, "degree limit exceeded");
        return -1;
    }
    *out = result;
    return 0;
}

static int parse_unary(cas_parser_t *ps, fcas_poly_t *out)
{
    skip_ws(ps);
    if (*ps->p == '+' || *ps->p == '-') {
        int neg = *ps->p == '-';
        ps->p++;
        if (parse_unary(ps, out) != 0) {
            return -1;
        }
        if (neg && poly_neg(out) != 0) {
            set_err(ps, "integer overflow");
            return -1;
        }
        return 0;
    }
    return parse_power(ps, out);
}

static int parse_term(cas_parser_t *ps, fcas_poly_t *out)
{
    if (parse_unary(ps, out) != 0) {
        return -1;
    }
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '*' && *ps->p != '/') {
            return 0;
        }
        int is_div = *ps->p == '/';
        ps->p++;
        fcas_poly_t rhs;
        if (parse_unary(ps, &rhs) != 0) {
            return -1;
        }
        fcas_poly_t result;
        if (is_div) {
            if (poly_div_const(out, &rhs, &result) != 0) {
                set_err(ps, "division by zero or polynomial");
                return -1;
            }
        } else if (poly_mul(out, &rhs, &result) != 0) {
            set_err(ps, "degree limit or integer overflow");
            return -1;
        }
        *out = result;
    }
}

static int parse_expr(cas_parser_t *ps, fcas_poly_t *out)
{
    if (parse_term(ps, out) != 0) {
        return -1;
    }
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '+' && *ps->p != '-') {
            return 0;
        }
        int sub = *ps->p == '-';
        ps->p++;
        fcas_poly_t rhs;
        if (parse_term(ps, &rhs) != 0) {
            return -1;
        }
        fcas_poly_t result;
        if (poly_add(out, &rhs, &result, sub) != 0) {
            set_err(ps, "degree limit or integer overflow");
            return -1;
        }
        *out = result;
    }
}

int fcas_parse(const char *expr, fcas_poly_t *out, char *err, size_t errcap)
{
    if (!expr || !out) {
        return FINTE_ERR_INVAL;
    }
    if (err && errcap) {
        err[0] = 0;
    }
    cas_parser_t ps;
    ps.start = expr;
    ps.p = expr;
    ps.err = err;
    ps.errcap = errcap;
    ps.failed = 0;
    ps.depth = 0;
    fcas_poly_t result;
    poly_zero(&result);
    skip_ws(&ps);
    if (!*ps.p) {
        set_err(&ps, "empty expression");
        return FINTE_ERR_INVAL;
    }
    if (parse_expr(&ps, &result) != 0 || ps.failed) {
        return FINTE_ERR_INVAL;
    }
    skip_ws(&ps);
    if (*ps.p) {
        set_err(&ps, "unexpected character (use explicit '*')");
        return FINTE_ERR_INVAL;
    }
    *out = result;
    return 0;
}

int fcas_diff(const fcas_poly_t *in, fcas_poly_t *out)
{
    if (!in || !out) {
        return FINTE_ERR_INVAL;
    }
    fcas_poly_t r;
    poly_zero(&r);
    for (int i = 1; i <= in->degree; i++) {
        int64_t n;
        if (__builtin_mul_overflow(in->num[i], (int64_t)i, &n) ||
            set_frac(&r, i - 1, n, in->den[i]) != 0) {
            return FINTE_ERR_LIMIT;
        }
    }
    if (poly_normalize(&r) != 0) {
        return FINTE_ERR_LIMIT;
    }
    *out = r;
    return 0;
}

int fcas_eval(const fcas_poly_t *p, int64_t xnum, int64_t xden,
              int64_t *onum, int64_t *oden)
{
    if (!p || !onum || !oden || xden == 0) {
        return FINTE_ERR_INVAL;
    }
    if (xden < 0) {
        if (xnum == INT64_MIN) {
            return FINTE_ERR_LIMIT;
        }
        xnum = -xnum;
        xden = -xden;
    }
    int64_t rn = 0;
    int64_t rd = 1;
    for (int i = p->degree; i >= 0; i--) {
        int64_t n;
        int64_t d;
        if (mul_frac(rn, rd, xnum, xden, &n, &d) != 0 ||
            add_frac(n, d, p->num[i], p->den[i], &n, &d) != 0) {
            return FINTE_ERR_LIMIT;
        }
        rn = n;
        rd = d;
    }
    fcas_poly_t tmp;
    poly_zero(&tmp);
    if (set_frac(&tmp, 0, rn, rd) != 0) {
        return FINTE_ERR_LIMIT;
    }
    *onum = tmp.num[0];
    *oden = tmp.den[0];
    return 0;
}

static int append_str(char *buf, size_t cap, size_t *used, const char *s)
{
    size_t n = finte_strlen(s);
    if (*used + n >= cap) {
        return -1;
    }
    finte_memcpy(buf + *used, s, n);
    *used += n;
    buf[*used] = 0;
    return 0;
}

static int append_fmt(char *buf, size_t cap, size_t *used,
                      const char *fmt, ...)
{
    if (*used >= cap) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || *used + (size_t)n >= cap) {
        return -1;
    }
    *used += (size_t)n;
    return 0;
}

int fcas_format(const fcas_poly_t *p, char *buf, size_t cap)
{
    if (!p || !buf || cap == 0) {
        return FINTE_ERR_INVAL;
    }
    buf[0] = 0;
    if (p->degree < 0) {
        if (cap < 2) {
            return FINTE_ERR_LIMIT;
        }
        buf[0] = '0';
        buf[1] = 0;
        return 0;
    }
    size_t used = 0;
    int first = 1;
    for (int i = p->degree; i >= 0; i--) {
        if (p->num[i] == 0) {
            continue;
        }
        int64_t n = p->num[i];
        int64_t d = p->den[i];
        int negative = n < 0;
        uint64_t mag = abs_u64(n);
        if (first) {
            if (negative && append_str(buf, cap, &used, "-") != 0) {
                return FINTE_ERR_LIMIT;
            }
        } else {
            if (append_str(buf, cap, &used,
                           negative ? " - " : " + ") != 0) {
                return FINTE_ERR_LIMIT;
            }
        }
        first = 0;
        if (i == 0 || mag != 1 || d != 1) {
            if (d == 1) {
                if (append_fmt(buf, cap, &used, "%llu",
                               (unsigned long long)mag) != 0) {
                    return FINTE_ERR_LIMIT;
                }
            } else if (append_fmt(buf, cap, &used, "%llu/%lld",
                                  (unsigned long long)mag,
                                  (long long)d) != 0) {
                return FINTE_ERR_LIMIT;
            }
            if (i > 0 && append_str(buf, cap, &used, "*") != 0) {
                return FINTE_ERR_LIMIT;
            }
        }
        if (i > 0) {
            if (append_str(buf, cap, &used, "x") != 0) {
                return FINTE_ERR_LIMIT;
            }
            if (i > 1 && append_fmt(buf, cap, &used, "^%d", i) != 0) {
                return FINTE_ERR_LIMIT;
            }
        }
    }
    return 0;
}
