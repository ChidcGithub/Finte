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
    switch (status) {
    case FINTE_OK: return "ok";
    case FINTE_ERR_INVAL: return "invalid";
    case FINTE_ERR_NOENT: return "no-entry";
    case FINTE_ERR_NOMEM: return "no-memory";
    case FINTE_ERR_PERM: return "permission-denied";
    case FINTE_ERR_BUSY: return "busy";
    case FINTE_ERR_AGAIN: return "would-block";
    case FINTE_ERR_LIMIT: return "limit";
    case FINTE_ERR_IO: return "io-error";
    case FINTE_ERR_DEADLOCK: return "deadlock";
    case FINTE_ERR_NOSYS: return "not-implemented";
    case FINTE_ERR_FAULT: return "fault";
    case FINTE_ERR_RANGE: return "out-of-range";
    case FINTE_ERR_EXIST: return "exists";
    case FINTE_ERR_TYPE: return "wrong-type";
    case FINTE_ERR_STATE: return "bad-state";
    case FINTE_ERR_EOF: return "end-of-stream";
    default: return "unknown";
    }
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

int finte_vformat(char *out, size_t cap, const char *fmt, va_list ap)
{
    fout_t o;
    o.buf = out;
    o.cap = cap;
    o.len = 0;

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
        switch (c) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            int len = (int)finte_strlen(s);
            int pad = width > len ? width - len : 0;
            while (pad-- > 0) {
                out_ch(&o, ' ');
            }
            out_str(&o, s);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            while (pad-- > 0) {
                out_ch(&o, ' ');
            }
            out_ch(&o, ch);
            break;
        }
        case 'd': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            if (v < 0) {
                out_ch(&o, '-');
                out_uint(&o, (uint64_t)(-(v + 1)) + 1u, 10, 0,
                         width > 0 ? width - 1 : 0, zero);
            } else {
                out_uint(&o, (uint64_t)v, 10, 0, width, zero);
            }
            break;
        }
        case 'u': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long)
                                       : va_arg(ap, unsigned int);
            out_uint(&o, v, 10, 0, width, zero);
            break;
        }
        case 'x': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long)
                                       : va_arg(ap, unsigned int);
            out_uint(&o, v, 16, 0, width, zero);
            break;
        }
        case 'X': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long)
                                       : va_arg(ap, unsigned int);
            out_uint(&o, v, 16, 1, width, zero);
            break;
        }
        case 'p':
            out_str(&o, "0x");
            out_uint(&o, (uintptr_t)va_arg(ap, void *), 16, 0, 0, 0);
            break;
        case '%':
            out_ch(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            out_ch(&o, '%');
            out_ch(&o, c);
            break;
        }
    }

    if (cap) {
        size_t at = o.len < cap - 1 ? o.len : cap - 1;
        out[at] = '\0';
    }
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
