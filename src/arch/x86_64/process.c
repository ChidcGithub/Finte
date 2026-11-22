/* x86_64 process backend: private PML4 roots, NX/read-only user pages,
 * CR3 switching, ring-3 entry and trap-frame formats. Shared process code is
 * in src/core/process.c. */
#include "finte/finte.h"
#include "finte/process.h"
#include "hal_x86.h"

#define PTE_P      0x1ull
#define PTE_RW     0x2ull
#define PTE_US     0x4ull
#define PTE_PWT    (1ull << 3)
#define PTE_PCD    (1ull << 4)
#define PTE_PS     0x80ull
#define PTE_NX     (1ull << 63)
#define PTE_ADDR   0x000FFFFFFFFFF000ull

typedef struct x86_as {
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *user_pd;
} x86_as_t;

static uint64_t g_kernel_pd[4][512] __attribute__((aligned(4096)));
static int g_kernel_pd_ready;
static uint64_t g_kernel_cr3;
static int g_nxe_ready[FINTE_MAX_CPUS];

extern void um_enter(uint64_t rip, uint64_t rsp, uint64_t flags)
    __attribute__((noreturn));
extern void um_enter_frame(void *frame, uint64_t kstack_top)
    __attribute__((noreturn));

static uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static uint64_t ptr_phys(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static void ensure_nxe(void)
{
    uint32_t id = hal_cpu_self()->id;
    if (id >= FINTE_MAX_CPUS) {
        id = 0;
    }
    if (g_nxe_ready[id]) {
        return;
    }
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= 1u << 11; /* EFER.NXE */
    __asm__ volatile("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi));
    g_nxe_ready[id] = 1;
}

static void kernel_pd_init(void)
{
    if (g_kernel_pd_ready) {
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 512; j++) {
            uint64_t addr = (uint64_t)i * 0x40000000ull +
                            (uint64_t)j * 0x200000ull;
            uint64_t flags = PTE_P | PTE_RW | PTE_PS;
            if (i == 3 && addr == 0xFEE00000ull) {
                flags |= PTE_PCD | PTE_PWT; /* LAPIC must not be WB */
            }
            g_kernel_pd[i][j] = addr | flags;
        }
    }
    g_kernel_pd_ready = 1;
}

static uint64_t *proc_pte(faddr_space_t *as, uint64_t va, int create)
{
    if (!as || !as->arch || va < FPROC_USER_BASE || va >= FPROC_USER_LIMIT) {
        return NULL;
    }
    x86_as_t *a = (x86_as_t *)as->arch;
    uint32_t pdi = (uint32_t)((va >> 21) & 0x1FFu);
    uint64_t pde = a->user_pd[pdi];
    if ((pde & PTE_P) == 0) {
        if (!create) {
            return NULL;
        }
        uint64_t *pt = (uint64_t *)finte_as_alloc_page(as);
        if (!pt) {
            return NULL;
        }
        a->user_pd[pdi] = ptr_phys(pt) | PTE_P | PTE_RW | PTE_US;
        pde = a->user_pd[pdi];
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR);
    return &pt[(va >> 12) & 0x1FFu];
}

static int x86_space_create(faddr_space_t *as)
{
    kernel_pd_init();
    ensure_nxe();
    x86_as_t *a = (x86_as_t *)hal_pages_alloc(4096);
    if (!a) {
        return -1;
    }
    finte_memset(a, 0, 4096);
    as->arch = a;
    a->pml4 = (uint64_t *)finte_as_alloc_page(as);
    a->pdpt = (uint64_t *)finte_as_alloc_page(as);
    a->user_pd = (uint64_t *)finte_as_alloc_page(as);
    if (!a->pml4 || !a->pdpt || !a->user_pd) {
        return -1;
    }
    a->pml4[0] = ptr_phys(a->pdpt) | PTE_P | PTE_RW | PTE_US;
    for (uint32_t i = 0; i < 4; i++) {
        a->pdpt[i] = ptr_phys(g_kernel_pd[i]) | PTE_P | PTE_RW;
    }
    a->pdpt[4] = ptr_phys(a->user_pd) | PTE_P | PTE_RW | PTE_US;
    as->root = ptr_phys(a->pml4);
    return 0;
}

static void x86_space_destroy(faddr_space_t *as)
{
    if (as && as->arch) {
        hal_pages_free(as->arch, 4096);
        as->arch = NULL;
    }
}

static uint32_t prot_from_pte(uint64_t pte)
{
    uint32_t prot = FPROC_PROT_READ;
    if (pte & PTE_RW) {
        prot |= FPROC_PROT_WRITE;
    }
    if ((pte & PTE_NX) == 0) {
        prot |= FPROC_PROT_EXEC;
    }
    return prot;
}

static int x86_space_clone(faddr_space_t *dst, faddr_space_t *src)
{
    if (!dst || !src || !dst->arch || !src->arch) {
        return -1;
    }
    x86_as_t *sa = (x86_as_t *)src->arch;
    for (uint32_t pdi = 0; pdi < 512; pdi++) {
        uint64_t pde = sa->user_pd[pdi];
        if ((pde & PTE_P) == 0) {
            continue;
        }
        uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR);
        for (uint32_t i = 0; i < 512; i++) {
            uint64_t pte = pt[i];
            if ((pte & (PTE_P | PTE_US)) != (PTE_P | PTE_US)) {
                continue;
            }
            uint64_t va = FPROC_USER_BASE +
                          (uint64_t)pdi * 0x200000ull +
                          (uint64_t)i * 4096ull;
            uint64_t pa = pte & PTE_ADDR;
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

static int x86_map_page(faddr_space_t *as, uint64_t va, uint64_t pa,
                        uint32_t prot)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 1);
    if (!pte) {
        return FINTE_ERR_NOMEM;
    }
    uint64_t flags = PTE_P | PTE_US;
    if (prot & FPROC_PROT_WRITE) {
        flags |= PTE_RW;
    }
    if (!(prot & FPROC_PROT_EXEC)) {
        flags |= PTE_NX;
    }
    *pte = (pa & PTE_ADDR) | flags;
    return 0;
}

static int x86_unmap_page(faddr_space_t *as, uint64_t va, uint64_t *pa)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 0);
    if (!pte || (*pte & PTE_P) == 0) {
        return -1;
    }
    if (pa) {
        *pa = *pte & PTE_ADDR;
    }
    *pte = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(va & ~0xFFFull) : "memory");
    return 0;
}

static uint64_t x86_translate(faddr_space_t *as, uint64_t va, int write)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 0);
    if (!pte || (*pte & (PTE_P | PTE_US)) != (PTE_P | PTE_US)) {
        return 0;
    }
    if (write && (*pte & PTE_RW) == 0) {
        return 0;
    }
    return (*pte & PTE_ADDR) | (va & 0xFFFull);
}

static uint32_t x86_prot(faddr_space_t *as, uint64_t va)
{
    uint64_t *pte = proc_pte(as, va & ~0xFFFull, 0);
    if (!pte || (*pte & (PTE_P | PTE_US)) != (PTE_P | PTE_US)) {
        return 0;
    }
    uint32_t prot = FPROC_PROT_READ;
    if (*pte & PTE_RW) {
        prot |= FPROC_PROT_WRITE;
    }
    if ((*pte & PTE_NX) == 0) {
        prot |= FPROC_PROT_EXEC;
    }
    return prot;
}

static uint64_t x86_root(faddr_space_t *as)
{
    return as ? as->root : 0;
}

static void x86_switch_root(faddr_space_t *as)
{
    ensure_nxe();
    if (as) {
        write_cr3(as->root);
    }
}

static uint64_t x86_kernel_root(void)
{
    if (g_kernel_cr3 == 0) {
        g_kernel_cr3 = read_cr3();
    }
    return g_kernel_cr3;
}

static void x86_flush_space(faddr_space_t *as)
{
    (void)as;
    write_cr3(read_cr3());
}

static void x86_enter_user(uint64_t entry, uint64_t usp, uint64_t kstack_top)
    __attribute__((noreturn));
static void x86_enter_user(uint64_t entry, uint64_t usp, uint64_t kstack_top)
{
    ensure_nxe();
    usermode_prepare_cpu(hal_cpu_self()->id);
    hal_user_bind_stack((void *)(uintptr_t)kstack_top);
    um_enter(entry, usp, 0x202u);
}

static void x86_exec_frame(void *frame, uint64_t entry, uint64_t sp)
{
    uint64_t *regs = (uint64_t *)frame;
    regs[14] = 0;  /* RAX */
    regs[15] = entry;
    regs[18] = sp;
}

static uint32_t x86_frame_size(void)
{
    return 160;
}

static void x86_fork_frame(void *dst, const void *src)
{
    finte_memcpy(dst, src, 160);
    ((uint64_t *)dst)[14] = 0; /* child returns 0 from fork (RAX slot) */
}

static int x86_signal_setup(faddr_space_t *as, void *frame, uint64_t handler,
                            uint64_t restorer, uint32_t sig)
{
    uint64_t *regs = (uint64_t *)frame;
    uint64_t *iret = (uint64_t *)((uint8_t *)frame + 120);
    regs[9] = sig;      /* RDI */
    iret[0] = handler;  /* RIP */
    uint64_t sp = iret[3] - 8u;
    uint64_t pa = finte_as_translate(as, sp, 1);
    if (!pa) {
        return -1;
    }
    *(uint64_t *)(uintptr_t)pa = restorer;
    iret[3] = sp;
    return 0;
}

static uint64_t x86_signal_restore(void *frame, const void *saved)
{
    uint64_t ret = ((const uint64_t *)saved)[14];
    finte_memcpy(frame, saved, 160);
    return ret;
}

static const fproc_arch_ops_t g_x86_ops = {
    .elf_machine = 62,
    .name = "x86_64",
    .space_create = x86_space_create,
    .space_destroy = x86_space_destroy,
    .space_clone = x86_space_clone,
    .map_page = x86_map_page,
    .unmap_page = x86_unmap_page,
    .translate = x86_translate,
    .prot = x86_prot,
    .root = x86_root,
    .switch_root = x86_switch_root,
    .kernel_root = x86_kernel_root,
    .flush_space = x86_flush_space,
    .enter_user = x86_enter_user,
    .enter_frame = um_enter_frame,
    .exec_frame = x86_exec_frame,
    .prepare = usermode_prepare,
    .frame_size = x86_frame_size,
    .fork_frame = x86_fork_frame,
    .signal_setup = x86_signal_setup,
    .signal_restore = x86_signal_restore,
};

const fproc_arch_ops_t *fproc_arch(void)
{
    ensure_nxe();
    (void)x86_kernel_root();
    return &g_x86_ops;
}
