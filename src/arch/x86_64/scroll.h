#ifndef FINTE_SCROLL_H
#define FINTE_SCROLL_H

#include <stddef.h>
#include <stdint.h>

/* Text history shared by the VGA and GOP consoles.
 *
 * sb_write() mirrors every character the console drivers receive.  When the
 * user scrolls back, the visible screen is repainted from this history; the
 * live drivers keep their own cursor state and resume painting at the bottom. */
void sb_init(int cols, int rows, int is_uefi);
void sb_write(const char *buf, size_t len);
void sb_clear(void);
void sb_clear_line(void);

int sb_cols(void);
int sb_rows(void);

/* delta < 0 scrolls back, delta > 0 returns towards live. Returns the offset
 * from live (0 means the live screen is shown). */
int sb_scroll(int delta);
int sb_scroll_top(void);
int sb_scroll_bottom(void);
int sb_offset(void);
void sb_return_live(void);

#endif /* FINTE_SCROLL_H */
