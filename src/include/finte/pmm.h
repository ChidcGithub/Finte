#ifndef FINTE_PMM_H
#define FINTE_PMM_H

#include <stddef.h>
#include "finte/boot.h"

#define FINTE_PAGE_SIZE 4096u
#define FINTE_PMM_MAX_PAGES (1u << 20) /* 4 GiB at 4 KiB */

/* Initialize from Boot Protocol v1. Returns 0 when a memory map was usable. */
int pmm_init(const fboot_info_t *bi, uint64_t min_addr, uint64_t max_addr);
int pmm_active(void);

/* Physical page allocation. Returns 0 on failure. */
uint64_t pmm_alloc_pages(size_t pages);
void pmm_free_pages(uint64_t addr, size_t pages);

/* Mark a physical range as reserved (kernel, modules, tables, firmware). */
int pmm_reserve(uint64_t addr, size_t bytes);

/* Range/introspection helpers. */
int pmm_owns(uint64_t addr, size_t bytes);
uint64_t pmm_base(void);
uint64_t pmm_end(void);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);
uint64_t pmm_usable_pages(void);
uint64_t pmm_reserved_pages(void);

#endif /* FINTE_PMM_H */
