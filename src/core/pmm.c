#include "finte/pmm.h"
#include "finte/hal.h"

#define PMM_BITMAP_BYTES ((FINTE_PMM_MAX_PAGES + 7u) / 8u)

static uint8_t g_used[PMM_BITMAP_BYTES];
static uint8_t g_alloc[PMM_BITMAP_BYTES];
static uint64_t g_base;
static uint64_t g_end;
static uint32_t g_pages;
static uint32_t g_hint;
static uint32_t g_free;
static uint32_t g_usable;
static uint32_t g_reserved;
static int g_active;
static volatile int g_lock;

static uint64_t pmm_lock(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_lock);
    return flags;
}

static void pmm_unlock(uint64_t flags)
{
    hal_spin_unlock(&g_lock);
    hal_irq_restore(flags);
}

static int bit_get(const uint8_t *bits, uint32_t i)
{
    return (bits[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint8_t *bits, uint32_t i)
{
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static void bit_clear(uint8_t *bits, uint32_t i)
{
    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static uint64_t align_up(uint64_t v)
{
    return (v + FINTE_PAGE_SIZE - 1u) & ~(uint64_t)(FINTE_PAGE_SIZE - 1u);
}

static uint64_t align_down(uint64_t v)
{
    return v & ~(uint64_t)(FINTE_PAGE_SIZE - 1u);
}

int pmm_init(const fboot_info_t *bi, uint64_t min_addr, uint64_t max_addr)
{
    g_active = 0;
    g_base = 0;
    g_end = 0;
    g_pages = 0;
    g_hint = 0;
    g_free = 0;
    g_usable = 0;
    g_reserved = 0;
    if (!bi || (bi->flags & FINTE_BOOT_FLAG_MEMMAP) == 0 ||
        bi->memmap_count == 0 ||
        bi->memmap_desc_size != sizeof(fboot_mmap_entry_t)) {
        return -1;
    }

    uint64_t lo = align_up(min_addr);
    uint64_t hi = align_down(max_addr);
    if (hi <= lo) {
        return -1;
    }
    if ((hi - lo) / FINTE_PAGE_SIZE > FINTE_PMM_MAX_PAGES) {
        hi = lo + (uint64_t)FINTE_PMM_MAX_PAGES * FINTE_PAGE_SIZE;
    }
    g_base = lo;
    g_end = hi;
    g_pages = (uint32_t)((hi - lo) / FINTE_PAGE_SIZE);

    /* Everything starts reserved/used. Only usable map entries become free. */
    for (uint32_t i = 0; i < PMM_BITMAP_BYTES; i++) {
        g_used[i] = 0xFF;
        g_alloc[i] = 0;
    }

    const fboot_mmap_entry_t *map =
        (const fboot_mmap_entry_t *)(uintptr_t)bi->memmap_addr;
    for (uint32_t e = 0; e < bi->memmap_count; e++) {
        if (map[e].type != FINTE_BOOT_MMAP_USABLE ||
            map[e].base + map[e].length < map[e].base) {
            continue;
        }
        uint64_t s = align_up(map[e].base);
        uint64_t en = align_down(map[e].base + map[e].length);
        if (s < g_base) {
            s = g_base;
        }
        if (en > g_end) {
            en = g_end;
        }
        if (en <= s) {
            continue;
        }
        uint32_t first = (uint32_t)((s - g_base) / FINTE_PAGE_SIZE);
        uint32_t last = (uint32_t)((en - g_base) / FINTE_PAGE_SIZE);
        for (uint32_t i = first; i < last; i++) {
            if (bit_get(g_used, i)) {
                bit_clear(g_used, i);
                g_free++;
                g_usable++;
            }
        }
    }
    if (g_free == 0) {
        g_pages = 0;
        return -1;
    }
    g_active = 1;
    return 0;
}

int pmm_active(void)
{
    return g_active;
}

uint64_t pmm_base(void)
{
    return g_base;
}

uint64_t pmm_end(void)
{
    return g_end;
}

uint64_t pmm_total_pages(void)
{
    return g_pages;
}

uint64_t pmm_free_page_count(void)
{
    return g_free;
}

uint64_t pmm_usable_pages(void)
{
    return g_usable;
}

uint64_t pmm_reserved_pages(void)
{
    return g_reserved;
}

int pmm_reserve(uint64_t addr, size_t bytes)
{
    if (!g_active || bytes == 0 || addr + (uint64_t)bytes < addr) {
        return -1;
    }
    uint64_t s = align_down(addr);
    uint64_t e = align_up(addr + (uint64_t)bytes);
    if (e <= g_base || s >= g_end) {
        return -1;
    }
    if (s < g_base) {
        s = g_base;
    }
    if (e > g_end) {
        e = g_end;
    }
    if (e <= s) {
        return -1;
    }
    uint32_t first = (uint32_t)((s - g_base) / FINTE_PAGE_SIZE);
    uint32_t last = (uint32_t)((e - g_base) / FINTE_PAGE_SIZE);
    uint64_t flags = pmm_lock();
    for (uint32_t i = first; i < last; i++) {
        if (!bit_get(g_used, i)) {
            bit_set(g_used, i);
            g_free--;
            g_reserved++;
        } else if (bit_get(g_alloc, i)) {
            bit_clear(g_alloc, i);
            bit_set(g_used, i);
        }
    }
    pmm_unlock(flags);
    return 0;
}

uint64_t pmm_alloc_pages(size_t pages)
{
    if (!g_active || pages == 0 || pages > g_pages) {
        return 0;
    }
    uint64_t flags = pmm_lock();
    uint32_t run = 0;
    uint32_t run_start = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass == 0 ? g_hint : 0;
        uint32_t end = pass == 0 ? g_pages : g_hint;
        for (uint32_t i = start; i < end; i++) {
            if (!bit_get(g_used, i)) {
                if (run == 0) {
                    run_start = i;
                }
                run++;
                if (run == pages) {
                    for (uint32_t j = 0; j < pages; j++) {
                        bit_set(g_used, run_start + j);
                        bit_set(g_alloc, run_start + j);
                    }
                    g_free -= (uint32_t)pages;
                    g_hint = run_start + (uint32_t)pages;
                    if (g_hint >= g_pages) {
                        g_hint = 0;
                    }
                    uint64_t out = g_base + (uint64_t)run_start * FINTE_PAGE_SIZE;
                    pmm_unlock(flags);
                    return out;
                }
            } else {
                run = 0;
            }
        }
        run = 0;
    }
    pmm_unlock(flags);
    return 0;
}

void pmm_free_pages(uint64_t addr, size_t pages)
{
    if (!g_active || pages == 0 || pages > g_pages ||
        (addr & (FINTE_PAGE_SIZE - 1u))) {
        return;
    }
    uint64_t bytes = (uint64_t)pages * FINTE_PAGE_SIZE;
    if (bytes / FINTE_PAGE_SIZE != pages || addr < g_base ||
        bytes > g_end - addr) {
        return;
    }
    uint32_t first = (uint32_t)((addr - g_base) / FINTE_PAGE_SIZE);
    uint64_t flags = pmm_lock();
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t p = first + i;
        if (bit_get(g_alloc, p)) {
            bit_clear(g_alloc, p);
            bit_clear(g_used, p);
            g_free++;
        }
    }
    if (first < g_hint) {
        g_hint = first;
    }
    pmm_unlock(flags);
}

int pmm_owns(uint64_t addr, size_t bytes)
{
    if (!g_active || bytes == 0 || (addr & (FINTE_PAGE_SIZE - 1u))) {
        return 0;
    }
    if (addr < g_base || addr > g_end) {
        return 0;
    }
    uint64_t end = addr + (uint64_t)bytes;
    return end >= addr && end <= g_end;
}
