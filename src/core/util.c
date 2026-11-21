#include "finte/base.h"

void *finte_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *finte_memset(void *dst, int value, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) {
        *d++ = (uint8_t)value;
    }
    return dst;
}

int finte_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

size_t finte_strlen(const char *s)
{
    size_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

size_t finte_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (!dst || cap == 0) {
        return src ? finte_strlen(src) : 0;
    }
    if (src) {
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
    return src ? finte_strlen(src) : 0;
}

int finte_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int finte_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

const char *finte_strstatus(int32_t status)
{
    static const char *const names[] = {
        "ok",
        "invalid",
        "no-entry",
        "no-memory",
        "permission-denied",
        "busy",
        "would-block",
        "limit",
        "io-error",
        "deadlock",
        "not-implemented",
        "fault",
        "out-of-range",
        "exists",
        "wrong-type",
        "bad-state",
        "end-of-stream"
    };
    if (status > 0 || status <= -(int32_t)FINTE_ARRAY_SIZE(names)) {
        return "unknown";
    }
    return names[-status];
}

typedef struct fout {
    char *buf;
    size_t cap;
    size_t len;
} fout_t;

static void out_ch(fout_t *o, char c)
{
    if (o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_str(fout_t *o, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        out_ch(o, *s++);
    }
}

static void out_uint(fout_t *o, uint64_t value, unsigned base, int upper,
                     int width, int zero)
{
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    int pad = width > n ? width - n : 0;
    while (pad-- > 0) {
        out_ch(o, zero ? '0' : ' ');
    }
    while (n > 0) {
        out_ch(o, tmp[--n]);
    }
}

typedef struct fmt_ctx {
    fout_t *out;
    va_list *ap;
    int width;
    int zero;
    int lng;
} fmt_ctx_t;

typedef void (*fmt_handler_t)(fmt_ctx_t *ctx);

static void fmt_s(fmt_ctx_t *ctx)
{
    const char *s = va_arg(*ctx->ap, const char *);
    if (!s) {
        s = "(null)";
    }
    int len = (int)finte_strlen(s);
    int pad = ctx->width > len ? ctx->width - len : 0;
    while (pad-- > 0) {
        out_ch(ctx->out, ' ');
    }
    out_str(ctx->out, s);
}

static void fmt_c(fmt_ctx_t *ctx)
{
    char ch = (char)va_arg(*ctx->ap, int);
    int pad = ctx->width > 1 ? ctx->width - 1 : 0;
    while (pad-- > 0) {
        out_ch(ctx->out, ' ');
    }
    out_ch(ctx->out, ch);
}

static void fmt_d(fmt_ctx_t *ctx)
{
    long long v = ctx->lng ? va_arg(*ctx->ap, long long)
                           : va_arg(*ctx->ap, int);
    if (v < 0) {
        out_ch(ctx->out, '-');
        out_uint(ctx->out, (uint64_t)(-(v + 1)) + 1u, 10, 0,
                 ctx->width > 0 ? ctx->width - 1 : 0, ctx->zero);
    } else {
        out_uint(ctx->out, (uint64_t)v, 10, 0, ctx->width, ctx->zero);
    }
}

static void fmt_u(fmt_ctx_t *ctx)
{
    unsigned long long v = ctx->lng
        ? va_arg(*ctx->ap, unsigned long long)
        : va_arg(*ctx->ap, unsigned int);
    out_uint(ctx->out, v, 10, 0, ctx->width, ctx->zero);
}

static void fmt_x(fmt_ctx_t *ctx)
{
    unsigned long long v = ctx->lng
        ? va_arg(*ctx->ap, unsigned long long)
        : va_arg(*ctx->ap, unsigned int);
    out_uint(ctx->out, v, 16, 0, ctx->width, ctx->zero);
}

static void fmt_x_upper(fmt_ctx_t *ctx)
{
    unsigned long long v = ctx->lng
        ? va_arg(*ctx->ap, unsigned long long)
        : va_arg(*ctx->ap, unsigned int);
    out_uint(ctx->out, v, 16, 1, ctx->width, ctx->zero);
}

static void fmt_p(fmt_ctx_t *ctx)
{
    out_str(ctx->out, "0x");
    out_uint(ctx->out, (uintptr_t)va_arg(*ctx->ap, void *), 16, 0, 0, 0);
}

static void fmt_percent(fmt_ctx_t *ctx)
{
    out_ch(ctx->out, '%');
}

static const fmt_handler_t fmt_handlers[128] = {
    ['s'] = fmt_s,
    ['c'] = fmt_c,
    ['d'] = fmt_d,
    ['u'] = fmt_u,
    ['x'] = fmt_x,
    ['X'] = fmt_x_upper,
    ['p'] = fmt_p,
    ['%'] = fmt_percent,
};

int finte_vformat(char *out, size_t cap, const char *fmt, va_list ap)
{
    fout_t o;
    o.buf = out;
    o.cap = cap;
    o.len = 0;
    va_list work;
    va_copy(work, ap);

    while (fmt && *fmt) {
        char c = *fmt++;
        if (c != '%') {
            out_ch(&o, c);
            continue;
        }
        c = *fmt++;
        int zero = 0;
        int width = 0;
        if (c == '0') {
            zero = 1;
            c = *fmt++;
        }
        while (c >= '0' && c <= '9') {
            width = width * 10 + (c - '0');
            if (width > 64) {
                width = 64;
            }
            c = *fmt++;
        }
        int lng = 0;
        while (c == 'l' || c == 'z') {
            lng++;
            c = *fmt++;
        }
        if (c == '\0') {
            fmt--;
            break;
        }
        unsigned char index = (unsigned char)c;
        if (index < FINTE_ARRAY_SIZE(fmt_handlers) &&
            fmt_handlers[index]) {
            fmt_ctx_t fctx = { &o, &work, width, zero, lng };
            fmt_handlers[index](&fctx);
        } else {
            out_ch(&o, '%');
            out_ch(&o, c);
        }
    }

    if (cap) {
        size_t at = o.len < cap - 1 ? o.len : cap - 1;
        out[at] = '\0';
    }
    va_end(work);
    return (int)o.len;
}

int finte_format(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = finte_vformat(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
