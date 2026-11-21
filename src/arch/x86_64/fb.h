#ifndef FINTE_FB_H
#define FINTE_FB_H

#include <stddef.h>
#include <stdint.h>
#include "finte/boot.h"

void fb_init(const fboot_info_t *bi);
int fb_ready(void);
void fb_clear(void);
void fb_write(const char *buf, size_t len);
void fb_clear_line(void);

int fb_text_cols(void);
int fb_text_rows(void);
void fb_render_cell(uint32_t x, uint32_t y, unsigned char ch,
                    uint8_t fg, uint8_t bg);
void fb_rows_touch(void);

#endif
