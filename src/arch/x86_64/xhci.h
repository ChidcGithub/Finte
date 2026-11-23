#ifndef FINTE_XHCI_H
#define FINTE_XHCI_H

#include "finte/hal.h"

void xhci_init(void);
int xhci_trykey(void);

int xhci_blk_count(void);
int xhci_blk_info(int index, fblk_info_t *out);
int xhci_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors);
int xhci_blk_write(int index, uint64_t lba, const void *buf, uint32_t sectors);

#endif
