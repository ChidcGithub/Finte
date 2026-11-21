/* 80x25 VGA text console for the BIOS/CSM path.
 * SGR escape sequences are parsed for foreground/background colors. */
#include "vga.h"
#include "finte/ansi.h"
#include "finte/base.h"

#define VGA_COLS 80
#define VGA_ROWS 25

static volatile uint16_t *const vga_mem = (volatile uint16_t *)0xB8000;
static int vga_row;
static int vga_col;
static ansi_parser_t vga_ansi;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t vga_attr(void)
{
    uint8_t fg = ansi_vga_color(ansi_fg_index(&vga_ansi.style));
    uint8_t bg = ansi_vga_color(vga_ansi.style.bg) & 0x07;
    return (uint8_t)(fg | (bg << 4));
}

static void vga_reset_color(void)
{
    ansi_init(&vga_ansi, 7);
}

static void vga_cursor(void)
{
    uint16_t pos = (uint16_t)(vga_row * VGA_COLS + vga_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_clear(void)
{
    vga_reset_color();
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_mem[i] = (uint16_t)(' ' | (0x07 << 8));
    }
    vga_row = 0;
    vga_col = 0;
    vga_cursor();
}

void vga_clear_line(void)
{
    for (int i = 0; i < VGA_COLS; i++) {
        vga_mem[vga_row * VGA_COLS + i] =
            (uint16_t)(' ' | (0x07 << 8));
    }
    vga_col = 0;
    vga_cursor();
}

void vga_init(void)
{
    vga_clear();
}

int vga_cols(void)
{
    return VGA_COLS;
}

int vga_rows(void)
{
    return VGA_ROWS;
}

void vga_render_cell(int x, int y, unsigned char ch, uint8_t fg, uint8_t bg)
{
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return;
    }
    uint8_t f = ansi_vga_color(fg);
    uint8_t b = (uint8_t)(ansi_vga_color(bg) & 0x07);
    vga_mem[y * VGA_COLS + x] =
        (uint16_t)((uint8_t)ch | ((uint8_t)(f | (b << 4)) << 8));
}

void vga_cursor_hide(void)
{
    outb(0x3D4, 0x0A);
    outb(0x3D5, (uint8_t)(inb(0x3D5) | 0x20));
}

void vga_cursor_live(void)
{
    outb(0x3D4, 0x0A);
    outb(0x3D5, (uint8_t)(inb(0x3D5) & ~0x20));
    vga_cursor();
}

static void vga_scroll(void)
{
    for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++) {
        vga_mem[i] = vga_mem[i + VGA_COLS];
    }
    for (int i = 0; i < VGA_COLS; i++) {
        vga_mem[VGA_COLS * (VGA_ROWS - 1) + i] =
            (uint16_t)(' ' | (0x07 << 8));
    }
    vga_row = VGA_ROWS - 1;
    vga_col = 0;
}

static void vga_putc(char c)
{
    uint8_t attr = vga_attr();
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
        } else if (vga_row > 0) {
            vga_row--;
            vga_col = VGA_COLS - 1;
        }
        vga_mem[vga_row * VGA_COLS + vga_col] =
            (uint16_t)(' ' | (0x07 << 8));
    } else if (c == '\t') {
        int next = (vga_col + 4) & ~3;
        while (vga_col < next && vga_col < VGA_COLS) {
            vga_putc(' ');
        }
        return;
    } else {
        vga_mem[vga_row * VGA_COLS + vga_col] =
            (uint16_t)((uint8_t)c | (attr << 8));
        vga_col++;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    }
    if (vga_row >= VGA_ROWS) {
        vga_scroll();
    }
}

void vga_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (!ansi_feed(&vga_ansi, c)) {
            vga_putc((char)c);
        }
    }
    vga_cursor();
}
