/* x86_64 platform hooks that depend on the firmware hand-off. */
#ifndef FINTE_HAL_X86_H
#define FINTE_HAL_X86_H

#include "finte/boot.h"
#include "finte/hal.h"

void hal_boot_info(const fboot_info_t *bi);
int hal_is_uefi(void);
void hal_user_bind_stack(void *top);

#endif /* FINTE_HAL_X86_H */
