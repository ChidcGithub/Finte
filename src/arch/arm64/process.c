/* arm64 process backend: per-process L0/L1/L2/L3 tables, ASIDs, EL0 entry
 * and trap-frame/PTE formats. Shared ELF/syscall/ABI code lives in
 * src/core/process.c. */
#include "finte/finte.h"
#include "finte/process.h"

#define APTE_VALID      0x1ull
#define APTE_TABLE      0x3ull
#define APTE_SH_INNER   (3ull << 8)
#define APTE_AF         (1ull << 10)
#define APTE_NG         (1ull << 11)
#define APTE_AP_EL0_RW  (1ull << 6)
#define APTE_AP_RO      (1ull << 7)
#define APTE_PXN        (1ull << 53)
#define APTE_UXN        (1ull << 54)
#define APTE_ADDR_MASK  0x0000FFFFFFFFF000ull
#define APTE_USER_FLAGS (APTE_TABLE | APTE_SH_INNER | APTE_AF | APTE_NG | \
                         APTE_AP_EL0_RW | APTE_PXN)

typedef struct arm_as {
    uint64_t *l0;
    uint64_t *l1;
    uint64_t *l2;
} arm_as_t;

#define ARM64_ASID_MAX 255u
static uint8_t g_asid_used[(ARM64_ASID_MAX + 8u) / 8u];
static volatile int g_asid_lock;

extern uint64_t arm64_mmu_ttbr0;
extern uint64_t *arm64_mmu_kernel_l1(void);
extern int arm64_mmu_va_bits(void);
extern void arm64_mmu_set_ttbr0(uint64_t token);
extern void arm64_mmu_flush_asid(uint32_t asid);
extern void arm64_mmu_flush_all(void);
extern void arm64_um_enter(uint64_t entry, uint64_t usp,
                           uint64_t kstack_top) __attribute__((noreturn));
extern void arm64_um_enter_frame(void *frame, uint64_t kstack_top)
    __attribute__((noreturn));

static uint32_t asid_alloc(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_asid_lock);
    for (uint32_t i = 1; i <= ARM64_ASID_MAX; i++) {
        uint8_t mask = (uint8_t)(1u << (i & 7u));
        if (!(g_asid_used[i >> 3] & mask)) {
            g_asid_used[i >> 3] |= mask;
            hal_spin_unlock(&g_asid_lock);
            hal_irq_restore(flags);
            return i;
        }
    }
    /* ASID rollover: drop every translation before reusing ASID 1. */
    arm64_mmu_flush_all();
    finte_memset(g_asid_used, 0, sizeof(g_asid_used));
    g_asid_used[0] = 2u;
    hal_spin_unlock(&g_asid_lock);
    hal_irq_restore(flags);
    return 1;
}

static void asid_free(uint32_t asid)
{
    if (asid == 0) {
        return;
    }
    /* Flush before the bit is reusable, so a new space cannot be allocated
     * the same ASID and then have its fresh TLB entries invalidated. */
    arm64_mmu_flush_asid(asid);
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_asid_lock);
    g_asid_used[asid >> 3] &= (uint8_t)~(1u << (asid & 7u));
    hal_spin_unlock(&g_asid_lock);
    hal_irq_restore(flags);
}

static uint64_t ptr_phys(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static uint64_t *proc_pte(faddr_space_t *as, uint64_t va, int create)
{
    if (!as || !as->arch || va < FPROC_USER_BASE || va >= FPROC_USER_LIMIT) {
        return NULL;
    }
    arm_as_t *a = (arm_as_t *)as->arch;
    if (!a->l2) {
        return NULL;
    }
    uint32_t l2i = (uint32_t)((va >> 21) & 0x1FFu);
    uint64_t l2e = a->l2[l2i];
    if ((l2e & APTE_TABLE) != APTE_TABLE) {
        if (!create) {
            return NULL;
        }
        uint64_t *l3 = (uint64_t *)finte_as_alloc_page(as);
        if (!l3) {
            return NULL;
        }
        a->l2[l2i] = ptr_phys(l3) | APTE_TABLE;
        l2e = a->l2[l2i];
    }
    uint64_t *l3 = (uint64_t *)(uintptr_t)(l2e & APTE_ADDR_MASK);
    return &l3[(va >> 12) & 0x1FFu];
}

static int arm_space_create(faddr_space_t *as)
{
    arm_as_t *a = (arm_as_t *)hal_pages_alloc(4096);
    if (!a) {
        return -1;
    }
    finte_memset(a, 0, 4096);
    a->l1 = (uint64_t *)finte_as_alloc_page(as);
    a->l2 = (uint64_t *)finte_as_alloc_page(as);
    if (!a->l1 || !a->l2) {
        return -1;
    }
    finte_memcpy(a->l1, arm64_mmu_kernel_l1(), 4096);
    a->l1[4] = ptr_phys(a->l2) | APTE_TABLE;
    if (arm64_mmu_va_bits() >= 48) {
        a->l0 = (uint64_t *)finte_as_alloc_page(as);
        if (!a->l0) {
            return -1;
        }
        a->l0[0] = ptr_phys(a->l1) | APTE_TABLE;
        as->root = ptr_phys(a->l0);
    } else {
        a->l0 = NULL;
        as->root = ptr_phys(a->l1);
    }
    as->arch = a;
    as->asid = asid_alloc();
    if (!as->asid) {
        return -1;
    }
    as->root = ((uint64_t)as->asid << 48) |
               (as->root & 0x0000FFFFFFFFF000ull);
    return 0;
}

static void arm_space_destroy(faddr_space_t *as)
{
    if (!as) {
        return;
    }
    asid_free(as->asid);
    as->asid = 0;
    if (as->arch) {
        hal_pages_free(as->arch, 4096);
        as->arch = NULL;
    }
}

static uint32_t prot_from_pte(uint64_t pte)
{
    uint32_t prot = FPROC_PROT_READ;
    if ((pte & APTE_AP_RO) == 0) {
        prot |= FPROC_PROT_WRITE;
    }
    if ((pte & APTE_UXN) == 0) {
        prot |= FPROC_PROT_EXEC;
    }
    return prot;
}

static int arm_space_clone(faddr_space_t *dst, faddr_space_t *src)
{
    if (!dst || !src || !dst->arch || !src->arch) {
        return -1;
    }
    arm_as_t *sa = (arm_as_t *)src->arch;
    for (uint32_t l2i = 0; l2i < 512; l2i++) {
        uint64_t l2e = sa->l2[l2i];
        if ((l2e & APTE_TABLE) != APTE_TABLE) {
            continue;
        }
        uint64_t *l3 = (uint64_t *)(uintptr_t)(l2e & APTE_ADDR_MASK);
        for (uint32_t i = 0; i < 512; i++) {
            uint64_t pte = l3[i];
            if ((pte & APTE_TABLE) != APTE_TABLE ||
                (pte & APTE_AP_EL0_RW) == 0) {
                continue;
            }
            uint64_t va = FPROC_USER_BASE +
                          (uint64_t)l2i * 0x200000ull +
                          (uint64_t)i * 4096ull;
            uint64_t pa = pte & APTE_ADDR_MASK;
            void *fresh = finte_as_alloc_page(dst);
            if (!fresh) {
                return -1;
            }
            finte_memcpy(fresh, (const void *)(uintptr_t)pa, 4096);
            if (finte_as_map(dst, va, ptr_phys(fresh),
                             prot_from_pte(pte)) != 0) {
                finte_as_free_page(dst, fresh);
                return -1;
            }
        }
    }
    return 0;
}

static int arm_map_page(faddr_space_t *as, uint64_t va, uint64_t pa,
                        uint32_t prot)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 1);
    if (!pte) {
        return FINTE_ERR_NOMEM;
    }
    uint64_t flags = APTE_USER_FLAGS;
    if (!(prot & FPROC_PROT_EXEC)) {
        flags |= APTE_UXN;
    }
    if (!(prot & FPROC_PROT_WRITE)) {
        flags |= APTE_AP_RO;
    }
    *pte = (pa & APTE_ADDR_MASK) | flags;
    return 0;
}

static int arm_unmap_page(faddr_space_t *as, uint64_t va, uint64_t *pa)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 0);
    if (!pte || (*pte & APTE_TABLE) != APTE_TABLE) {
        return -1;
    }
    if (pa) {
        *pa = *pte & APTE_ADDR_MASK;
    }
    *pte = 0;
    arm64_mmu_flush_asid(as->asid);
    return 0;
}

static uint64_t arm_translate(faddr_space_t *as, uint64_t va, int write)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 0);
    if (!pte || (*pte & APTE_TABLE) != APTE_TABLE ||
        (*pte & APTE_AP_EL0_RW) == 0) {
        return 0;
    }
    if (write && (*pte & APTE_AP_RO)) {
        return 0;
    }
    return (*pte & APTE_ADDR_MASK) | (va & 0xFFFull);
}

static uint32_t arm_prot(faddr_space_t *as, uint64_t va)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 0);
    if (!pte || (*pte & APTE_TABLE) != APTE_TABLE ||
        (*pte & APTE_AP_EL0_RW) == 0) {
        return 0;
    }
    uint32_t prot = FPROC_PROT_READ;
    if ((*pte & APTE_AP_RO) == 0) {
        prot |= FPROC_PROT_WRITE;
    }
    if ((*pte & APTE_UXN) == 0) {
        prot |= FPROC_PROT_EXEC;
    }
    return prot;
}

static uint64_t arm_root(faddr_space_t *as)
{
    return as ? as->root : 0;
}

static void arm_switch_root(faddr_space_t *as)
{
    if (!as) {
        return;
    }
    /* Invalidate the incoming ASID before activating it. On hardware this
     * only drops that address space's entries; on QEMU the implementation
     * degrades to a full flush, which also makes the switch safe on TCG. */
    if (as->asid) {
        arm64_mmu_flush_asid(as->asid);
    }
    arm64_mmu_set_ttbr0(as->root);
}

static uint64_t arm_kernel_root(void)
{
    return arm64_mmu_ttbr0;
}

static void arm_flush_space(faddr_space_t *as)
{
    if (as) {
        arm64_mmu_flush_asid(as->asid);
    }
}

static void arm_user_prepare(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(1ull << 22); /* PAN off: kernel uses physical aliases */
    __asm__ volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr) : "memory");
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= 3ull << 20; /* FP/SIMD usable at EL0 */
    __asm__ volatile("msr cpacr_el1, %0; isb" : : "r"(cpacr) : "memory");
}

static void arm_fork_frame(void *dst, const void *src)
{
    finte_memcpy(dst, src, 288);
    ((uint64_t *)dst)[0] = 0; /* child returns 0 from fork */
}

static int arm_signal_setup(faddr_space_t *as, void *frame, uint64_t handler,
                            uint64_t restorer, uint32_t sig)
{
    (void)as;
    uint64_t *f = (uint64_t *)frame;
    f[0] = sig;        /* x0 */
    f[30] = restorer;  /* x30 */
    f[31] = handler;   /* ELR_EL1 */
    return 0;
}

static uint64_t arm_signal_restore(void *frame, const void *saved)
{
    uint64_t ret = ((const uint64_t *)saved)[0];
    finte_memcpy(frame, saved, 288);
    return ret;
}

static void arm_exec_frame(void *frame, uint64_t entry, uint64_t sp)
{
    uint64_t *f = (uint64_t *)frame;
    f[0] = 0;    /* x0 */
    f[31] = entry;
    f[35] = sp;  /* SP_EL0 */
}

static uint32_t arm_frame_size(void)
{
    return 288;
}

static const fproc_arch_ops_t g_arm_ops = {
    .elf_machine = 183,
    .name = "arm64",
    .space_create = arm_space_create,
    .space_destroy = arm_space_destroy,
    .space_clone = arm_space_clone,
    .map_page = arm_map_page,
    .unmap_page = arm_unmap_page,
    .translate = arm_translate,
    .prot = arm_prot,
    .root = arm_root,
    .switch_root = arm_switch_root,
    .kernel_root = arm_kernel_root,
    .flush_space = arm_flush_space,
    .enter_user = arm64_um_enter,
    .enter_frame = arm64_um_enter_frame,
    .exec_frame = arm_exec_frame,
    .prepare = arm_user_prepare,
    .frame_size = arm_frame_size,
    .fork_frame = arm_fork_frame,
    .signal_setup = arm_signal_setup,
    .signal_restore = arm_signal_restore,
};

const fproc_arch_ops_t *fproc_arch(void)
{
    return &g_arm_ops;
}
