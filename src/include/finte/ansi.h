/* Shared ANSI SGR parser and palette.
 *
 * VGA text, GOP framebuffer and the scrollback shadow grid all consume the
 * same byte stream. Keeping one parser means escape handling and color
 * semantics cannot drift between the three console backends. */
#ifndef FINTE_ANSI_H
#define FINTE_ANSI_H

#include <stdint.h>

typedef struct ansi_style {
    uint8_t fg;   /* ANSI index: 0-7 normal, 8-15 bright */
    uint8_t bg;   /* ANSI index: 0-7 */
    uint8_t bold;
} ansi_style_t;

typedef struct ansi_parser {
    uint8_t state;      /* 0 = text, 1 = ESC, 2 = CSI */
    int value;          /* current numeric parameter, -1 when empty */
    uint8_t default_fg; /* color restored by SGR 0 and SGR 39 */
    ansi_style_t style;
} ansi_parser_t;

void ansi_init(ansi_parser_t *p, uint8_t default_fg);
void ansi_reset_style(ansi_parser_t *p);

/* Consume one byte. Returns 1 when it was part of an escape sequence,
 * 0 when the caller should render it as text. */
int ansi_feed(ansi_parser_t *p, unsigned char c);

/* Effective foreground after applying bold, as an ANSI 0-15 index. */
uint8_t ansi_fg_index(const ansi_style_t *style);

/* Palette conversion for the two text renderers. */
uint8_t ansi_vga_color(uint8_t index);
uint32_t ansi_rgb(uint8_t index);

#endif /* FINTE_ANSI_H */
