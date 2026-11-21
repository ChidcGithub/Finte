#ifndef FINTE_INPUT_H
#define FINTE_INPUT_H

#include "finte/boot.h"

#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_LEFT 0x103
#define KEY_RIGHT 0x104
#define KEY_HOME 0x105
#define KEY_END 0x106
#define KEY_DELETE 0x107
#define KEY_PGUP 0x108
#define KEY_PGDN 0x109
#define KEY_SHIFT_UP 0x10A
#define KEY_SHIFT_DOWN 0x10B

void input_init(void);
void input_boot(const fboot_info_t *bi);
int input_trykey(void);

/* Returns wheel notches accumulated since the last call. Positive means the
 * wheel was rolled away from the user (scroll back). */
int input_trywheel(void);

#endif
