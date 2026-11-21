#ifndef FINTE_VGA_H
#define FINTE_VGA_H

#include <stddef.h>
#include <stdint.h>

void vga_init(void);
void vga_clear(void);
void vga_write(const char *buf, size_t len);
void vga_clear_line(void);

int vga_cols(void);
int vga_rows(void);
void vga_render_cell(int x, int y, unsigned char ch, uint8_t fg, uint8_t bg);
void vga_cursor_hide(void);
void vga_cursor_live(void);

#endif
