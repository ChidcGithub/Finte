#ifndef FINTE_INITRD_H
#define FINTE_INITRD_H

#include "finte/base.h"

#define FINTE_INITRD_MAGIC 0x46494E52u /* "FINR" */
#define FINTE_INITRD_MAX 32

int finte_initrd_init(const void *addr, size_t size);
uint32_t finte_initrd_count(void);
const char *finte_initrd_name(uint32_t index);
const void *finte_initrd_data(uint32_t index);
uint32_t finte_initrd_size(uint32_t index);
int finte_initrd_find(const char *name);

#endif
