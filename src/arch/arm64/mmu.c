/* Early AArch64 identity map for QEMU virt.
 *
 * 39-bit or 48-bit VA (from the firmware TCR), 4 KiB granule, 2 MiB identity
 * blocks. RAM and the PL011/GIC/virtio device window are mapped; everything
 * else is left invalid. The kernel runs from its link address, so the map is
 * deliberately simple and static.
 */
#include "finte/finte.h"

#define ARM64_RAM_DEFAULT_BASE 0x40000000ull
#define ARM64_RAM_DEFAULT_END  0x80000000ull
#define ARM64_DEV_BASE         0x08000000ull
#define ARM64_DEV_END          0x0A100000ull
#define ARM64_MAP_GB           4u

_Static_assert(sizeof(uint64_t) == 8, "AArch64 page tables need 64-bit entries");

static uint64_t g_l0[512] __attribute__((aligned(4096)));
static uint64_t g_l1[512] __attribute__((aligned(4096)));
static uint64_t g_l2[ARM64_MAP_GB][512] __attribute__((aligned(4096)));

uint64_t arm64_mmu_ttbr0;
uint64_t arm64_mmu_tcr;
uint64_t arm64_mmu_mair;
uint64_t arm64_mmu_sctlr;

static int g_mmu_on;

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

static int is_ram(const fboot_info_t *bi, uint64_t base, uint64_t len)
{
    if (bi && (bi->flags & FINTE_BOOT_FLAG_MEMMAP) && bi->memmap_addr) {
        const fboot_mmap_entry_t *map =
            (const fboot_mmap_entry_t *)(uintptr_t)bi->memmap_addr;
        for (uint32_t i = 0; i < bi->memmap_count; i++) {
            if (map[i].type == FINTE_BOOT_MMAP_USABLE &&
                overlaps(base, len, map[i].base, map[i].length)) {
                return 1;
            }
        }
        return 0;
    }
    return overlaps(base, len, ARM64_RAM_DEFAULT_BASE,
                    ARM64_RAM_DEFAULT_END - ARM64_RAM_DEFAULT_BASE);
}

void arm64_mmu_init(const fboot_info_t *bi)
{
    for (uint32_t i = 0; i < 512; i++) {
        g_l0[i] = 0;
        g_l1[i] = 0;
    }
    for (uint32_t g = 0; g < ARM64_MAP_GB; g++) {
        for (uint32_t i = 0; i < 512; i++) {
            g_l2[g][i] = 0;
        }
    }

    g_l0[0] = (uint64_t)(uintptr_t)g_l1 | 0x3u;
    for (uint32_t g = 0; g < ARM64_MAP_GB; g++) {
        g_l1[g] = (uint64_t)(uintptr_t)g_l2[g] | 0x3u;
        for (uint32_t i = 0; i < 512; i++) {
            uint64_t base = (uint64_t)g * 0x40000000ull +
                            (uint64_t)i * 0x200000ull;
            uint64_t len = 0x200000ull;
            if (overlaps(base, len, ARM64_DEV_BASE,
                         ARM64_DEV_END - ARM64_DEV_BASE)) {
                /* Device-nGnRnE, AF, EL1 RW, no execute. */
                g_l2[g][i] = base | 0x1u | (1u << 2) | (1u << 10) |
                             (1ull << 53) | (1ull << 54);
            } else if (is_ram(bi, base, len)) {
                /* Normal WB, inner shareable, AF, EL1 RW, executable. */
                g_l2[g][i] = base | 0x1u | (3u << 8) | (1u << 10);
            }
        }
    }

    uint64_t mair = 0xFFull;             /* Attr0 normal WB */
    mair |= 0x00ull << 8;                /* Attr1 device nGnRnE */
    arm64_mmu_mair = mair;
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair) : "memory");

    /* Keep the current root depth. Firmware may use 48-bit VA (4 levels,
     * TTBR0 -> L0); direct QEMU boot may leave TCR_EL1 at reset (also 48-bit).
     * If firmware uses 39-bit VA, use the 3-level L1 root instead. */
    uint64_t old_tcr;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(old_tcr));
    int use_48 = (old_tcr & 0x3Full) < 25u;
    uint64_t tsz = use_48 ? 16ull : 25ull;
    uint64_t tcr = tsz;                  /* T0SZ */
    tcr |= 1ull << 8;                    /* IRGN0 WB */
    tcr |= 1ull << 10;                   /* ORGN0 WB */
    tcr |= 3ull << 12;                   /* SH0 inner shareable */
    tcr |= tsz << 16;                    /* T1SZ */
    tcr |= 0ull << 30;                   /* TG1 4K */
    tcr |= 2ull << 32;                   /* IPS 40-bit */
    tcr |= 1ull << 23;                   /* EPD1: no TTBR1 walks */
    /* T0SZ=16 is a 48-bit VA (L0 root); T0SZ=25 is 39-bit (L1 root). */
    uint64_t ttbr0 = (uint64_t)(uintptr_t)(use_48 ? g_l0 : g_l1);
    arm64_mmu_ttbr0 = ttbr0;
    arm64_mmu_tcr = tcr;
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"(ttbr0) : "memory");
    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr) : "memory");
    __asm__ volatile("msr ttbr1_el1, xzr" : : : "memory");

    __asm__ volatile("dsb sy; tlbi vmalle1; dsb sy; isb" : : : "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12); /* M, C, I */
    sctlr &= ~(1ull << 1);                              /* no alignment check */
    sctlr &= ~(1ull << 19);                             /* WXN off */
    arm64_mmu_sctlr = sctlr;
    __asm__ volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr) : "memory");
    g_mmu_on = 1;
}

int arm64_mmu_enabled(void)
{
    return g_mmu_on;
}

int arm64_mmu_va_bits(void)
{
    return 64 - (int)(arm64_mmu_tcr & 0x3Full);
}
