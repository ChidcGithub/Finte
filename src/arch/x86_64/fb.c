/* Linear framebuffer text console for UEFI GOP.
 *
 * The framebuffer is usually uncached MMIO, so per-pixel stores are expensive.
 * Glyphs are drawn as packed 64-bit words, and rows are cleared only when they
 * are actually reused. Boot output is append-only, so most rows need no clear.
 */
#include "fb.h"
#include "finte/ansi.h"
#include "font8x8.h"
#include "finte/base.h"

#define FB_MAX_ROWS 128

static uint8_t *fb_mem;
static uint32_t fb_pitch;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_cols;
static uint32_t fb_rows;
static uint32_t fb_scale;
static uint32_t fb_line;
static uint32_t fb_col;
static uint32_t fb_row;
static int fb_valid;
static uint8_t fb_row_used[FB_MAX_ROWS];
static uint32_t fb_fg = 0x00FFFFFFu;
static uint32_t fb_bg = 0;
static ansi_parser_t fb_ansi;

static void fb_apply_style(void)
{
    fb_fg = ansi_rgb(ansi_fg_index(&fb_ansi.style));
    fb_bg = ansi_rgb(fb_ansi.style.bg);
}

static void fb_fill_rows(uint32_t y0, uint32_t y1, uint32_t color)
{
    uint64_t pair = ((uint64_t)color << 32) | color;
    for (uint32_t y = y0; y < y1 && y < fb_height; y++) {
        volatile uint64_t *line =
            (volatile uint64_t *)(fb_mem + (size_t)y * fb_pitch);
        uint32_t qwords = fb_width / 2;
        for (uint32_t i = 0; i < qwords; i++) {
            line[i] = pair;
        }
        if (fb_width & 1u) {
            volatile uint32_t *tail =
                (volatile uint32_t *)(fb_mem + (size_t)y * fb_pitch +
                                      (size_t)(fb_width - 1) * 4);
            *tail = color;
        }
    }
}

static void fb_clear_row(uint32_t text_row)
{
    uint32_t y0 = text_row * fb_line;
    uint32_t y1 = y0 + fb_line;
    fb_fill_rows(y0, y1, fb_bg);
    if (text_row < FB_MAX_ROWS) {
        fb_row_used[text_row] = 0;
    }
}

void fb_clear(void)
{
    if (!fb_valid) {
        return;
    }
    ansi_init(&fb_ansi, 15);
    fb_apply_style();
    fb_fill_rows(0, fb_height, 0);
    for (uint32_t i = 0; i < FB_MAX_ROWS; i++) {
        fb_row_used[i] = 0;
    }
    fb_col = 0;
    fb_row = 0;
}

void fb_clear_line(void)
{
    if (!fb_valid) {
        return;
    }
    fb_clear_row(fb_row);
    fb_col = 0;
}

void fb_init(const fboot_info_t *bi)
{
    fb_valid = 0;
    if (!bi || bi->fb_addr == 0 || bi->fb_width < 320 || bi->fb_height < 200 ||
        bi->fb_pitch < bi->fb_width * 4 || bi->fb_bpp != 32) {
        return;
    }
    fb_mem = (uint8_t *)(uintptr_t)bi->fb_addr;
    fb_pitch = bi->fb_pitch;
    fb_width = bi->fb_width;
    fb_height = bi->fb_height;
    fb_scale = fb_width >= 800 ? 2u : 1u;
    fb_cols = fb_width / (8u * fb_scale);
    fb_line = 8u * fb_scale + 2u;
    fb_rows = fb_height / fb_line;
    if (fb_cols == 0 || fb_rows == 0 || fb_rows > FB_MAX_ROWS) {
        return;
    }
    fb_valid = 1;
    fb_clear();
}

int fb_ready(void)
{
    return fb_valid;
}

int fb_text_cols(void)
{
    return (int)fb_cols;
}

int fb_text_rows(void)
{
    return (int)fb_rows;
}

void fb_rows_touch(void)
{
    for (uint32_t i = 0; i < FB_MAX_ROWS; i++) {
        fb_row_used[i] = 1;
    }
}

static void fb_scroll(void)
{
    /* Hardware scans the framebuffer top to bottom, so the text must really
     * move up one row. dst < src, so a forward copy is overlap-safe. */
    uint32_t step = fb_line;
    size_t bytes = (size_t)(fb_height - step) * fb_pitch;
    uint8_t *dst = fb_mem;
    const uint8_t *src = fb_mem + (size_t)step * fb_pitch;
    size_t qwords = bytes / sizeof(uint64_t);
    volatile uint64_t *d = (volatile uint64_t *)(void *)dst;
    volatile uint64_t *s = (volatile uint64_t *)(const void *)src;
    for (size_t i = 0; i < qwords; i++) {
        d[i] = s[i];
    }
    for (size_t i = qwords * sizeof(uint64_t); i < bytes; i++) {
        dst[i] = src[i];
    }
    for (uint32_t i = 0; i + 1 < fb_rows && i + 1 < FB_MAX_ROWS; i++) {
        fb_row_used[i] = fb_row_used[i + 1];
    }
    fb_clear_row(fb_rows - 1);
    fb_row = fb_rows - 1;
    fb_col = 0;
}

static void fb_draw_char(char ch)
{
    unsigned idx = (unsigned char)ch;
    if (idx >= 128) {
        idx = '?';
    }
    uint32_t x0 = fb_col * 8u * fb_scale;
    uint32_t y0 = fb_row * fb_line;
    uint32_t pixels = 8u * fb_scale;
    uint32_t qwords = pixels / 2u;

    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        uint64_t packed[8];
        for (uint32_t q = 0; q < qwords; q++) {
            uint32_t p0 = fb_bg;
            uint32_t p1 = fb_bg;
            for (uint32_t k = 0; k < 2; k++) {
                uint32_t px = q * 2u + k;
                uint32_t bit = px / fb_scale;
                uint32_t color = (bits & (1u << bit)) ? fb_fg : fb_bg;
                if (k == 0) {
                    p0 = color;
                } else {
                    p1 = color;
                }
            }
            packed[q] = ((uint64_t)p1 << 32) | p0;
        }
        for (uint32_t sy = 0; sy < fb_scale; sy++) {
            volatile uint64_t *dst =
                (volatile uint64_t *)(fb_mem +
                                      (size_t)(y0 + row * fb_scale + sy) *
                                          fb_pitch +
                                      (size_t)x0 * 4);
            for (uint32_t q = 0; q < qwords; q++) {
                dst[q] = packed[q];
            }
        }
    }
}

void fb_render_cell(uint32_t x, uint32_t y, unsigned char ch,
                    uint8_t fg, uint8_t bg)
{
    if (!fb_valid || x >= fb_cols || y >= fb_rows) {
        return;
    }
    uint32_t saved_col = fb_col;
    uint32_t saved_row = fb_row;
    uint32_t saved_fg = fb_fg;
    uint32_t saved_bg = fb_bg;
    fb_fg = ansi_rgb(fg);
    fb_bg = ansi_rgb(bg);
    fb_col = x;
    fb_row = y;
    fb_draw_char((char)ch);
    fb_col = saved_col;
    fb_row = saved_row;
    fb_fg = saved_fg;
    fb_bg = saved_bg;
}

static void fb_putc(char ch)
{
    fb_apply_style();
    if (fb_col == 0 && ch != '\n' && ch != '\r' && ch != '\b' && ch != '\t' &&
        fb_row < FB_MAX_ROWS && !fb_row_used[fb_row]) {
        /* The row is already blank; avoid a full-width clear. */
    } else if (fb_col == 0 && ch != '\n' && ch != '\r' && ch != '\b' &&
               ch != '\t') {
        fb_clear_row(fb_row);
    }
    if (ch == '\n') {
        fb_col = 0;
        fb_row++;
    } else if (ch == '\r') {
        fb_col = 0;
    } else if (ch == '\b') {
        if (fb_col > 0) {
            fb_col--;
        } else if (fb_row > 0) {
            fb_row--;
            fb_col = fb_cols - 1;
        }
        fb_draw_char(' ');
    } else if (ch == '\t') {
        uint32_t next = (fb_col + 4) & ~3u;
        while (fb_col < next && fb_col < fb_cols) {
            fb_putc(' ');
        }
        return;
    } else {
        fb_draw_char(ch);
        if (fb_row < FB_MAX_ROWS) {
            fb_row_used[fb_row] = 1;
        }
        fb_col++;
        if (fb_col >= fb_cols) {
            fb_col = 0;
            fb_row++;
        }
    }
    if (fb_row >= fb_rows) {
        fb_scroll();
    }
}

void fb_write(const char *buf, size_t len)
{
    if (!fb_valid) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (!ansi_feed(&fb_ansi, c)) {
            fb_putc((char)c);
        }
    }
}
