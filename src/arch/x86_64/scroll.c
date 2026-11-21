/* Shared console text history and scrollback.
 *
 * The VGA and GOP drivers each own a live cursor and paint as output arrives.
 * This module keeps a shadow grid of the same characters and colors.  A
 * scroll request repaints the visible window from the grid; the bottom row
 * becomes a small status bar while scrolled.  Returning to live repaints the
 * tail and restores the VGA cursor.
 */
#include "scroll.h"
#include "finte/ansi.h"
#include "fb.h"
#include "vga.h"
#include "finte/base.h"

#define SB_MAX_COLS 256
#define SB_MAX_LINES 512

typedef struct sb_cell {
    uint8_t ch;
    uint8_t fg; /* ANSI index: 0-7 normal, 8-15 bright */
    uint8_t bg; /* ANSI index: 0-7 */
    uint8_t pad;
} sb_cell_t;

static sb_cell_t sb_grid[SB_MAX_LINES][SB_MAX_COLS];
static int sb_width = 80;
static int sb_height = 25;
static int sb_is_uefi;
static int sb_ready;

static uint32_t sb_cur; /* absolute index of the line being written */
static int sb_col;
static int sb_view = -1; /* absolute top line while scrolled; -1 = live */
static ansi_parser_t sb_ansi;

static const sb_cell_t sb_blank = { ' ', 7, 0, 0 };

static sb_cell_t *sb_line(uint32_t abs)
{
    return sb_grid[abs % SB_MAX_LINES];
}

static void sb_fill_line(sb_cell_t *line, int from, int to)
{
    for (int i = from; i < to; i++) {
        line[i] = sb_blank;
    }
}

static int sb_first_valid(void)
{
    if (sb_cur >= SB_MAX_LINES) {
        return (int)(sb_cur - SB_MAX_LINES + 1);
    }
    return 0;
}

static int sb_live_top(void)
{
    int top = (int)sb_cur - sb_height + 1;
    return top < 0 ? 0 : top;
}

static void sb_new_line(void)
{
    sb_cur++;
    sb_col = 0;
    sb_fill_line(sb_line(sb_cur), 0, sb_width);
    if (sb_view >= 0) {
        int first = sb_first_valid();
        if (sb_view < first) {
            sb_view = first;
        }
    }
}

static void sb_put(int x, uint8_t ch)
{
    if (x < 0 || x >= sb_width) {
        return;
    }
    sb_cell_t *c = &sb_line(sb_cur)[x];
    c->ch = ch;
    c->fg = sb_ansi.style.fg;
    c->bg = sb_ansi.style.bg;
    c->pad = sb_ansi.style.bold ? 1 : 0;
}

static void sb_putc(char ch)
{
    if (ch == '\n') {
        sb_new_line();
    } else if (ch == '\r') {
        sb_col = 0;
    } else if (ch == '\b') {
        if (sb_col > 0) {
            sb_col--;
        } else if (sb_cur > 0) {
            sb_cur--;
            sb_col = sb_width - 1;
        }
        sb_put(sb_col, ' ');
    } else if (ch == '\t') {
        int next = (sb_col + 4) & ~3;
        while (sb_col < next && sb_col < sb_width) {
            sb_put(sb_col, ' ');
            sb_col++;
        }
    } else {
        sb_put(sb_col, (uint8_t)ch);
        sb_col++;
        if (sb_col >= sb_width) {
            sb_new_line();
        }
    }
}

static void sb_draw_cell(int x, int y, const sb_cell_t *c)
{
    uint8_t fg = c->fg;
    if (c->pad && fg < 8) {
        fg = (uint8_t)(fg + 8);
    }
    if (sb_is_uefi) {
        fb_render_cell((uint32_t)x, (uint32_t)y, c->ch, fg, c->bg);
    } else {
        vga_render_cell(x, y, c->ch, fg, c->bg);
    }
}

static void sb_status_text(char *buf, size_t cap, int offset)
{
    finte_format(buf, cap,
                 " scroll +%d  PgUp/PgDn  Shift+Up/Down  wheel  "
                 "any key returns ", offset);
}

static void sb_draw_status(int offset)
{
    char text[96];
    sb_status_text(text, sizeof(text), offset);
    size_t n = finte_strlen(text);
    int y = sb_height - 1;
    for (int x = 0; x < sb_width; x++) {
        sb_cell_t c = sb_blank;
        c.fg = 0;
        c.bg = 6;
        if ((size_t)x < n) {
            c.ch = (uint8_t)text[x];
        }
        sb_draw_cell(x, y, &c);
    }
}

static void sb_paint(uint32_t top, int status, int offset)
{
    for (int y = 0; y < sb_height; y++) {
        if (status && y == sb_height - 1) {
            sb_draw_status(offset);
            continue;
        }
        uint32_t abs = top + (uint32_t)y;
        const sb_cell_t *line = sb_line(abs);
        for (int x = 0; x < sb_width; x++) {
            if (abs <= sb_cur) {
                sb_draw_cell(x, y, &line[x]);
            } else {
                sb_draw_cell(x, y, &sb_blank);
            }
        }
    }
    if (sb_is_uefi) {
        fb_rows_touch();
    } else if (status) {
        vga_cursor_hide();
    } else {
        vga_cursor_live();
    }
}

static void sb_live_paint(void)
{
    sb_view = -1;
    sb_paint((uint32_t)sb_live_top(), 0, 0);
}

void sb_init(int cols, int rows, int is_uefi)
{
    if (cols < 1) {
        cols = 80;
    }
    if (cols > SB_MAX_COLS) {
        cols = SB_MAX_COLS;
    }
    if (rows < 1) {
        rows = 25;
    }
    sb_width = cols;
    sb_height = rows;
    sb_is_uefi = is_uefi;
    sb_cur = 0;
    sb_col = 0;
    sb_view = -1;
    ansi_init(&sb_ansi, 7);
    for (int y = 0; y < SB_MAX_LINES; y++) {
        sb_fill_line(sb_grid[y], 0, sb_width);
    }
    sb_ready = 1;
}

void sb_write(const char *buf, size_t len)
{
    if (!sb_ready || !buf) {
        return;
    }
    int was_scrolled = sb_view >= 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (!ansi_feed(&sb_ansi, c)) {
            sb_putc((char)c);
        }
    }
    if (was_scrolled) {
        /* Keep the absolute view stable and refresh the offset readout. */
        sb_draw_status(sb_offset());
    }
}

void sb_clear(void)
{
    if (!sb_ready) {
        return;
    }
    for (int y = 0; y < SB_MAX_LINES; y++) {
        sb_fill_line(sb_grid[y], 0, sb_width);
    }
    sb_cur = 0;
    sb_col = 0;
    sb_view = -1;
    ansi_init(&sb_ansi, 7);
    if (!sb_is_uefi) {
        vga_cursor_live();
    }
}

void sb_clear_line(void)
{
    if (!sb_ready) {
        return;
    }
    sb_fill_line(sb_line(sb_cur), 0, sb_width);
    sb_col = 0;
}

int sb_cols(void)
{
    return sb_width;
}

int sb_rows(void)
{
    return sb_height;
}

int sb_offset(void)
{
    if (sb_view < 0) {
        return 0;
    }
    int offset = sb_live_top() - sb_view;
    return offset > 0 ? offset : 0;
}

int sb_scroll(int delta)
{
    if (!sb_ready) {
        return 0;
    }
    int live = sb_live_top();
    int first = sb_first_valid();
    int top = sb_view >= 0 ? sb_view : live;
    long next = (long)top + delta;
    if (next < first) {
        next = first;
    }
    if (next > live) {
        next = live;
    }
    if (next >= live) {
        if (sb_view >= 0) {
            sb_live_paint();
        }
        return 0;
    }
    sb_view = (int)next;
    int offset = live - sb_view;
    sb_paint((uint32_t)sb_view, 1, offset);
    return offset;
}

int sb_scroll_top(void)
{
    return sb_scroll(-(1 << 30));
}

int sb_scroll_bottom(void)
{
    if (sb_view >= 0) {
        sb_live_paint();
    }
    return 0;
}

void sb_return_live(void)
{
    if (sb_view >= 0) {
        sb_live_paint();
    }
}
