/* Shared ANSI SGR parser and palette. See finte/ansi.h. */
#include "finte/ansi.h"

static const uint8_t ansi_to_vga[16] = {
    0, 4, 2, 6, 1, 5, 3, 7,
    8, 12, 10, 14, 9, 13, 11, 15
};

static const uint32_t ansi_to_rgb[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
};

void ansi_reset_style(ansi_parser_t *p)
{
    p->style.fg = p->default_fg;
    p->style.bg = 0;
    p->style.bold = 0;
}

void ansi_init(ansi_parser_t *p, uint8_t default_fg)
{
    p->state = 0;
    p->value = -1;
    p->default_fg = (uint8_t)(default_fg & 0x0F);
    ansi_reset_style(p);
}

static void ansi_sgr(ansi_parser_t *p, int code)
{
    ansi_style_t *s = &p->style;
    if (code < 0 || code == 0) {
        ansi_reset_style(p);
    } else if (code == 1) {
        s->bold = 1;
    } else if (code == 22) {
        s->bold = 0;
    } else if (code >= 30 && code <= 37) {
        s->fg = (uint8_t)(code - 30);
    } else if (code >= 90 && code <= 97) {
        s->fg = (uint8_t)(code - 90 + 8);
    } else if (code == 39) {
        s->fg = p->default_fg;
    } else if (code >= 40 && code <= 47) {
        s->bg = (uint8_t)(code - 40);
    } else if (code == 49) {
        s->bg = 0;
    }
}

int ansi_feed(ansi_parser_t *p, unsigned char c)
{
    if (p->state == 0) {
        if (c != 0x1B) {
            return 0;
        }
        p->state = 1;
        return 1;
    }
    if (p->state == 1) {
        if (c == '[') {
            p->state = 2;
            p->value = -1;
        } else {
            p->state = 0;
        }
        return 1;
    }
    if (c >= '0' && c <= '9') {
        if (p->value < 100000) {
            p->value = (p->value < 0 ? 0 : p->value * 10) + (c - '0');
        }
        return 1;
    }
    if (c == ';') {
        ansi_sgr(p, p->value);
        p->value = -1;
        return 1;
    }
    if (c == 'm') {
        ansi_sgr(p, p->value);
        p->value = -1;
    }
    p->state = 0;
    p->value = -1;
    return 1;
}

uint8_t ansi_fg_index(const ansi_style_t *style)
{
    if (style->bold && style->fg < 8) {
        return (uint8_t)(style->fg + 8);
    }
    return style->fg;
}

uint8_t ansi_vga_color(uint8_t index)
{
    return ansi_to_vga[index & 0x0F];
}

uint32_t ansi_rgb(uint8_t index)
{
    return ansi_to_rgb[index & 0x0F];
}
