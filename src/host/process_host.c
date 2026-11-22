/* Host-only process backend: a flat page map for tests of the shared
 * address-space, mmap and fd layers. No user mode is entered on the host. */
#include "finte/finte.h"
#include "finte/process.h"

typedef struct host_map {
    uint64_t va;
    uint64_t pa;
    uint32_t prot;
} host_map_t;

typedef struct host_as {
    host_map_t map[128];
    uint32_t count;
} host_as_t;

static int host_space_create(faddr_space_t *as)
{
    host_as_t *a = (host_as_t *)hal_pages_alloc(4096);
    if (!a) {
        return -1;
    }
    finte_memset(a, 0, 4096);
    as->arch = a;
    as->root = 0x1000;
    return 0;
}

static void host_space_destroy(faddr_space_t *as)
{
    if (as && as->arch) {
        hal_pages_free(as->arch, 4096);
        as->arch = NULL;
    }
}

static int host_map_page(faddr_space_t *as, uint64_t va, uint64_t pa,
                         uint32_t prot);

static int host_space_clone(faddr_space_t *dst, faddr_space_t *src)
{
    if (!dst || !src || !dst->arch || !src->arch) {
        return -1;
    }
    host_as_t *d = (host_as_t *)dst->arch;
    host_as_t *s = (host_as_t *)src->arch;
    d->count = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        uint64_t va = s->map[i].va;
        uint64_t pa = s->map[i].pa;
        uint32_t pte_prot = s->map[i].prot;
        if (!finte_as_has_page(src, pa)) {
            if (host_map_page(dst, va, pa, pte_prot) != 0) {
                return -1;
            }
            continue;
        }
        uint32_t prot = finte_cow_prot(pa);
        if (!prot) {
            prot = pte_prot;
        }
        uint32_t map_prot = prot & ~FPROC_PROT_WRITE;
        if (finte_cow_share(pa, prot) != 0) {
            return -1;
        }
        if (finte_as_share_page(dst, pa) != 0) {
            (void)finte_cow_put(pa);
            return -1;
        }
        if (host_map_page(src, va, pa, map_prot) != 0 ||
            host_map_page(dst, va, pa, map_prot) != 0) {
            return -1;
        }
    }
    return 0;
}

static int host_map_page(faddr_space_t *as, uint64_t va, uint64_t pa,
                         uint32_t prot)
{
    if (!as || !as->arch) {
        return -1;
    }
    host_as_t *a = (host_as_t *)as->arch;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->map[i].va == (va & ~0xFFFull)) {
            a->map[i].pa = pa;
            a->map[i].prot = prot;
            return 0;
        }
    }
    if (a->count >= FINTE_ARRAY_SIZE(a->map)) {
        return -1;
    }
    a->map[a->count].va = va & ~0xFFFull;
    a->map[a->count].pa = pa;
    a->map[a->count].prot = prot;
    a->count++;
    return 0;
}

static int host_unmap_page(faddr_space_t *as, uint64_t va, uint64_t *pa)
{
    if (!as || !as->arch) {
        return -1;
    }
    host_as_t *a = (host_as_t *)as->arch;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->map[i].va == (va & ~0xFFFull)) {
            if (pa) {
                *pa = a->map[i].pa;
            }
            a->map[i] = a->map[a->count - 1u];
            a->count--;
            return 0;
        }
    }
    return -1;
}

static uint64_t host_translate(faddr_space_t *as, uint64_t va, int write)
{
    if (!as || !as->arch) {
        return 0;
    }
    host_as_t *a = (host_as_t *)as->arch;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->map[i].va == (va & ~0xFFFull)) {
            if (write && !(a->map[i].prot & FPROC_PROT_WRITE)) {
                return 0;
            }
            return a->map[i].pa + (va & 0xFFFull);
        }
    }
    return 0;
}

static uint32_t host_prot(faddr_space_t *as, uint64_t va)
{
    if (!as || !as->arch) {
        return 0;
    }
    host_as_t *a = (host_as_t *)as->arch;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->map[i].va == (va & ~0xFFFull)) {
            return a->map[i].prot;
        }
    }
    return 0;
}

static uint64_t host_root(faddr_space_t *as)
{
    return as ? as->root : 0;
}

static void host_switch_root(faddr_space_t *as)
{
    (void)as;
}

static uint64_t host_kernel_root(void)
{
    return 0x1000;
}

static void host_flush_space(faddr_space_t *as)
{
    (void)as;
}

static void host_enter_user(uint64_t entry, uint64_t usp, uint64_t kstack)
    __attribute__((noreturn));
static void host_enter_user(uint64_t entry, uint64_t usp, uint64_t kstack)
{
    (void)entry;
    (void)usp;
    (void)kstack;
    for (;;) {
    }
}

static uint32_t host_frame_size(void)
{
    return 0;
}

static void host_prepare(void)
{
}

static const fproc_arch_ops_t g_host_ops = {
    .elf_machine = 62,
    .name = "host",
    .space_create = host_space_create,
    .space_destroy = host_space_destroy,
    .space_clone = host_space_clone,
    .map_page = host_map_page,
    .unmap_page = host_unmap_page,
    .translate = host_translate,
    .prot = host_prot,
    .root = host_root,
    .switch_root = host_switch_root,
    .kernel_root = host_kernel_root,
    .flush_space = host_flush_space,
    .enter_user = host_enter_user,
    .exec_frame = NULL,
    .prepare = host_prepare,
    .frame_size = host_frame_size,
};

const fproc_arch_ops_t *fproc_arch(void)
{
    return &g_host_ops;
}
