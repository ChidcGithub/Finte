/* Shared user-process core for x86_64 and arm64.
 *
 * The architecture layer owns page tables, root switching, trap frames and
 * EL0/ring-3 entry. This file owns address-space refcounting, ELF loading,
 * user-copy, the native syscall ABI, file descriptors, mmap, process lifetime
 * and the user demos. */
#include "finte/finte.h"
#include "finte/initrd.h"
#include "finte/process.h"
#include "finte/service.h"
#include "finte/verb.h"

typedef struct __attribute__((packed)) elf64_ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

#define AS_BYTES ((sizeof(faddr_space_t) + 4095u) & ~(size_t)4095u)
#define PROC_BYTES ((sizeof(fproc_t) + 4095u) & ~(size_t)4095u)

#define DEMO_SHM_MAGIC 0xF17E5EED12345678ull
#define DEMO_IPC_FLAG    (1u << 0)
#define DEMO_ISO_FLAG    (1u << 1)
#define DEMO_SHM_FLAG    (1u << 2)
#define DEMO_TIME_FLAG   (1u << 3)
#define DEMO_PREEMPT_FLAG (1u << 4)
#define DEMO_ARENA_FLAG  (1u << 5)
#define DEMO_ARENA_DENY_FLAG (1u << 6)

typedef struct demo_shm {
    volatile uint64_t magic;
    volatile uint64_t heartbeat;
    volatile uint64_t flags;
    volatile uint64_t secret_addr;
    volatile uint64_t secret_value;
    volatile uint64_t elapsed_ns;
    volatile uint64_t spin_count;
    volatile uint64_t reserved;
} demo_shm_t;

static fproc_t *g_procs[FPROC_MAX_PROCS];
static uint32_t g_next_pid = 1;
static uint32_t g_live_spaces;
static volatile int g_proc_lock;

/* Copy-on-write registry: physical page -> (spaces mapping it, original
 * protection). Pages are shared read-only on clone; the first write makes a
 * private copy (or upgrades the last owner in place). */
#define FPROC_COW_MAX 4096

typedef struct fcow {
    uint64_t pa;
    uint32_t refs;
    uint32_t prot;
} fcow_t;

static fcow_t g_cow[FPROC_COW_MAX];
static volatile int g_cow_lock;

static uint64_t cow_lock(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_cow_lock);
    return flags;
}

static void cow_unlock(uint64_t flags)
{
    hal_spin_unlock(&g_cow_lock);
    hal_irq_restore(flags);
}

static fcow_t *cow_find(uint64_t pa)
{
    for (uint32_t i = 0; i < FPROC_COW_MAX; i++) {
        if (g_cow[i].prot && g_cow[i].pa == pa) {
            return &g_cow[i];
        }
    }
    return NULL;
}

uint32_t finte_cow_prot(uint64_t pa)
{
    uint64_t flags = cow_lock();
    fcow_t *e = cow_find(pa);
    uint32_t prot = e ? e->prot : 0;
    cow_unlock(flags);
    return prot;
}

int finte_cow_share(uint64_t pa, uint32_t prot)
{
    uint64_t flags = cow_lock();
    fcow_t *e = cow_find(pa);
    if (e) {
        e->refs++;
        cow_unlock(flags);
        return 0;
    }
    for (uint32_t i = 0; i < FPROC_COW_MAX; i++) {
        if (g_cow[i].prot == 0) {
            g_cow[i].pa = pa;
            g_cow[i].refs = 2; /* source space + clone */
            g_cow[i].prot = prot;
            cow_unlock(flags);
            return 0;
        }
    }
    cow_unlock(flags);
    return -1;
}

/* Drop one mapping reference. Returns 1 when the caller should free the
 * page, 0 while another space still maps it. */
int finte_cow_put(uint64_t pa)
{
    uint64_t flags = cow_lock();
    fcow_t *e = cow_find(pa);
    if (!e) {
        cow_unlock(flags);
        return 1;
    }
    if (e->refs > 1) {
        e->refs--;
        cow_unlock(flags);
        return 0;
    }
    finte_memset(e, 0, sizeof(*e));
    cow_unlock(flags);
    return 1;
}

int finte_as_has_page(faddr_space_t *as, uint64_t pa)
{
    if (!as) {
        return 0;
    }
    for (uint32_t i = 0; i < as->page_count; i++) {
        if (as->pages[i] == pa) {
            return 1;
        }
    }
    return 0;
}

int finte_as_share_page(faddr_space_t *as, uint64_t pa)
{
    if (!as || as->page_count >= FPROC_MAX_PAGES) {
        return -1;
    }
    if (finte_as_has_page(as, pa)) {
        return 0;
    }
    as->pages[as->page_count++] = pa;
    return 0;
}

int finte_as_forget_page(faddr_space_t *as, uint64_t pa)
{
    if (!as) {
        return -1;
    }
    for (uint32_t i = 0; i < as->page_count; i++) {
        if (as->pages[i] == pa) {
            as->pages[i] = as->pages[as->page_count - 1u];
            as->page_count--;
            return 0;
        }
    }
    return -1;
}

/* Resolve a write fault on a COW page. Returns 0 when the instruction can be
 * retried, -1 when the fault is not a COW write and must kill the process. */
int finte_as_cow_fault(faddr_space_t *as, uint64_t va, int write)
{
    if (!as || !write) {
        return -1;
    }
    uint64_t page = va & ~0xFFFull;
    uint64_t pa = finte_as_translate(as, page, 0);
    if (!pa) {
        return -1;
    }
    uint32_t prot = finte_cow_prot(pa);
    if (!prot || !(prot & FPROC_PROT_WRITE)) {
        return -1;
    }

    uint64_t flags = cow_lock();
    fcow_t *e = cow_find(pa);
    if (!e) {
        cow_unlock(flags);
        return -1;
    }
    uint32_t refs = e->refs;
    if (refs == 1) {
        /* Last mapping: upgrade the existing page in place. */
        finte_memset(e, 0, sizeof(*e));
        cow_unlock(flags);
        if (finte_as_map(as, page, pa, prot) != 0) {
            return -1;
        }
        finte_as_flush(as);
        return 0;
    }
    cow_unlock(flags);

    void *fresh = finte_as_alloc_page(as);
    if (!fresh) {
        return -1;
    }
    finte_memcpy(fresh, (const void *)(uintptr_t)pa, 4096);
    if (finte_as_map(as, page, (uint64_t)(uintptr_t)fresh, prot) != 0) {
        finte_as_free_page(as, fresh);
        return -1;
    }
    finte_as_forget_page(as, pa);
    if (finte_cow_put(pa)) {
        hal_pages_free((void *)(uintptr_t)pa, 4096);
    }
    finte_as_flush(as);
    return 0;
}
static faddr_space_t g_kernel_space;
static int g_kernel_space_ready;
static void proc_begin(void);

static void proc_abort_children(fproc_t *p)
{
    for (fproc_t *c = p ? p->children : NULL; c; c = c->next_sibling) {
        if (c->flow && c->flow->state != FINTE_FLOW_DEAD &&
            c->flow->state != FINTE_FLOW_FREE) {
            finte_proc_abort(c);
        }
    }
}

static int proc_live_children(fproc_t *p)
{
    for (fproc_t *c = p ? p->children : NULL; c; c = c->next_sibling) {
        if (c->flow && c->flow->state != FINTE_FLOW_DEAD &&
            c->flow->state != FINTE_FLOW_FREE) {
            return 1;
        }
    }
    return 0;
}

static uint64_t proc_lock(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_proc_lock);
    return flags;
}

static void proc_unlock(uint64_t flags)
{
    hal_spin_unlock(&g_proc_lock);
    hal_irq_restore(flags);
}

static void kernel_space_init(void)
{
    if (g_kernel_space_ready) {
        return;
    }
    const fproc_arch_ops_t *ops = fproc_arch();
    finte_memset(&g_kernel_space, 0, sizeof(g_kernel_space));
    g_kernel_space.ops = ops;
    g_kernel_space.root = ops->kernel_root();
    g_kernel_space.kernel_root = g_kernel_space.root;
    g_kernel_space_ready = 1;
}

uint32_t finte_as_live_spaces(void)
{
    return g_live_spaces;
}

faddr_space_t *finte_as_create(void)
{
    const fproc_arch_ops_t *ops = fproc_arch();
    if (!ops) {
        return NULL;
    }
    faddr_space_t *as = (faddr_space_t *)hal_pages_alloc(AS_BYTES);
    if (!as) {
        return NULL;
    }
    finte_memset(as, 0, sizeof(*as));
    as->refs = 1;
    as->ops = ops;
    if (ops->space_create(as) != 0) {
        if (ops->space_destroy) {
            ops->space_destroy(as);
        }
        for (uint32_t i = 0; i < as->page_count; i++) {
            hal_pages_free((void *)(uintptr_t)as->pages[i], 4096);
        }
        hal_pages_free(as, AS_BYTES);
        return NULL;
    }
    as->kernel_root = ops->kernel_root();
    __atomic_add_fetch(&g_live_spaces, 1, __ATOMIC_RELAXED);
    return as;
}

faddr_space_t *finte_as_clone(faddr_space_t *src)
{
    if (!src || !src->ops) {
        return NULL;
    }
    faddr_space_t *dst = (faddr_space_t *)hal_pages_alloc(AS_BYTES);
    if (!dst) {
        return NULL;
    }
    finte_memset(dst, 0, sizeof(*dst));
    dst->refs = 1;
    dst->ops = src->ops;
    if (src->ops->space_create(dst) != 0) {
        if (dst->ops->space_destroy) {
            dst->ops->space_destroy(dst);
        }
        hal_pages_free(dst, AS_BYTES);
        return NULL;
    }
    __atomic_add_fetch(&g_live_spaces, 1, __ATOMIC_RELAXED);
    if (src->ops->space_clone(dst, src) != 0) {
        finte_as_unref(dst);
        return NULL;
    }
    dst->kernel_root = src->kernel_root;
    /* The clone made the source's writable pages read-only. */
    finte_as_flush(src);
    return dst;
}

faddr_space_t *finte_as_ref(faddr_space_t *as)
{
    if (as) {
        __atomic_add_fetch(&as->refs, 1, __ATOMIC_RELAXED);
    }
    return as;
}

void finte_as_unref(faddr_space_t *as)
{
    if (!as) {
        return;
    }
    if (__atomic_sub_fetch(&as->refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    if (as->ops) {
        if (as->ops->flush_space) {
            as->ops->flush_space(as);
        }
        if (as->ops->space_destroy) {
            as->ops->space_destroy(as);
        }
    }
    for (uint32_t i = 0; i < as->page_count; i++) {
        uint64_t pa = as->pages[i];
        if (finte_cow_put(pa)) {
            hal_pages_free((void *)(uintptr_t)pa, 4096);
        }
    }
    as->page_count = 0;
    __atomic_sub_fetch(&g_live_spaces, 1, __ATOMIC_RELAXED);
    hal_pages_free(as, AS_BYTES);
}

void *finte_as_alloc_page(faddr_space_t *as)
{
    if (!as || as->page_count >= FPROC_MAX_PAGES) {
        return NULL;
    }
    void *page = hal_pages_alloc(4096);
    if (!page) {
        return NULL;
    }
    finte_memset(page, 0, 4096);
    as->pages[as->page_count++] = (uint64_t)(uintptr_t)page;
    return page;
}

void finte_as_free_page(faddr_space_t *as, void *page)
{
    if (!as || !page) {
        return;
    }
    uint64_t pa = (uint64_t)(uintptr_t)page;
    for (uint32_t i = 0; i < as->page_count; i++) {
        if (as->pages[i] == pa) {
            as->pages[i] = as->pages[as->page_count - 1u];
            as->page_count--;
            if (finte_cow_put(pa)) {
                hal_pages_free(page, 4096);
            }
            return;
        }
    }
}

int finte_as_map(faddr_space_t *as, uint64_t va, uint64_t pa, uint32_t prot)
{
    if (!as || !as->ops || !as->ops->map_page) {
        return FINTE_ERR_STATE;
    }
    return as->ops->map_page(as, va, pa, prot);
}

int finte_as_unmap(faddr_space_t *as, uint64_t va, uint64_t *pa)
{
    if (!as || !as->ops || !as->ops->unmap_page) {
        return FINTE_ERR_STATE;
    }
    return as->ops->unmap_page(as, va, pa);
}

uint64_t finte_as_translate(faddr_space_t *as, uint64_t va, int write)
{
    if (!as || !as->ops || !as->ops->translate) {
        return 0;
    }
    return as->ops->translate(as, va, write);
}

uint32_t finte_as_prot(faddr_space_t *as, uint64_t va)
{
    if (!as || !as->ops || !as->ops->prot) {
        return 0;
    }
    return as->ops->prot(as, va);
}

int finte_as_user_ok(faddr_space_t *as, uint64_t va, uint64_t len, int write)
{
    if (len == 0) {
        return 1;
    }
    if (va + len < va || !as) {
        return 0;
    }
    if (va < FPROC_USER_BASE || va + len > FPROC_USER_LIMIT) {
        return 0;
    }
    for (uint64_t page = va & ~0xFFFull; page < va + len; page += 4096u) {
        uint64_t pa = finte_as_translate(as, page, write);
        if (pa) {
            continue;
        }
        if (write) {
            /* A present read-only COW page can be made writable on copy. */
            uint64_t rpa = finte_as_translate(as, page, 0);
            if (rpa && (finte_cow_prot(rpa) & FPROC_PROT_WRITE)) {
                continue;
            }
        }
        return 0;
    }
    return 1;
}

uint64_t finte_as_root(faddr_space_t *as)
{
    return as ? as->root : 0;
}

void finte_as_switch(faddr_space_t *as)
{
    if (!as) {
        finte_as_switch_kernel();
        return;
    }
    if (as->ops && as->ops->switch_root) {
        as->ops->switch_root(as);
    }
}

void finte_as_switch_kernel(void)
{
    kernel_space_init();
    if (g_kernel_space.ops && g_kernel_space.ops->switch_root) {
        g_kernel_space.ops->switch_root(&g_kernel_space);
    }
}

void finte_as_flush(faddr_space_t *as)
{
    if (as && as->ops && as->ops->flush_space) {
        as->ops->flush_space(as);
    }
}

static fproc_t *proc_alloc(void)
{
    fproc_t *p = (fproc_t *)hal_pages_alloc(PROC_BYTES);
    if (p) {
        finte_memset(p, 0, PROC_BYTES);
    }
    return p;
}

static void proc_table_add(fproc_t *p)
{
    uint64_t flags = proc_lock();
    for (uint32_t i = 0; i < FPROC_MAX_PROCS; i++) {
        if (!g_procs[i]) {
            g_procs[i] = p;
            break;
        }
    }
    proc_unlock(flags);
}

static void proc_table_remove(fproc_t *p)
{
    uint64_t flags = proc_lock();
    for (uint32_t i = 0; i < FPROC_MAX_PROCS; i++) {
        if (g_procs[i] == p) {
            g_procs[i] = NULL;
            break;
        }
    }
    proc_unlock(flags);
}

fproc_t *finte_proc_find(uint32_t pid)
{
    uint64_t flags = proc_lock();
    fproc_t *found = NULL;
    for (uint32_t i = 0; i < FPROC_MAX_PROCS; i++) {
        if (g_procs[i] && g_procs[i]->pid == pid) {
            found = g_procs[i];
            break;
        }
    }
    proc_unlock(flags);
    return found;
}

static int proc_pgid_exists(uint32_t pgid)
{
    uint64_t flags = proc_lock();
    int found = 0;
    for (uint32_t i = 0; i < FPROC_MAX_PROCS; i++) {
        if (g_procs[i] && g_procs[i]->pgid == pgid) {
            found = 1;
            break;
        }
    }
    proc_unlock(flags);
    return found;
}

static int proc_collect_pgid(uint32_t pgid, fproc_t **out, uint32_t max)
{
    uint64_t flags = proc_lock();
    uint32_t n = 0;
    for (uint32_t i = 0; i < FPROC_MAX_PROCS && n < max; i++) {
        if (g_procs[i] && g_procs[i]->pgid == pgid) {
            out[n++] = g_procs[i];
        }
    }
    proc_unlock(flags);
    return (int)n;
}

static uint32_t proc_alloc_pid(void)
{
    for (uint32_t tries = 0; tries < 0x10000u; tries++) {
        uint32_t pid = g_next_pid++;
        if (g_next_pid == 0) {
            g_next_pid = 1;
        }
        if (pid != 0 && !finte_proc_find(pid)) {
            return pid;
        }
    }
    return 0;
}

static int proc_load_elf(faddr_space_t *as, const char *name, uint64_t *entry)
{
    int idx = finte_initrd_find(name);
    if (idx < 0) {
        return FINTE_ERR_NOENT;
    }
    const uint8_t *image = (const uint8_t *)finte_initrd_data((uint32_t)idx);
    uint32_t size = finte_initrd_size((uint32_t)idx);
    if (size < sizeof(elf64_ehdr_t) || image[0] != 0x7F ||
        image[1] != 'E' || image[2] != 'L' || image[3] != 'F' ||
        image[4] != 2 || image[5] != 1) {
        return FINTE_ERR_TYPE;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (eh->machine != as->ops->elf_machine || eh->type != 2 ||
        eh->phentsize < sizeof(elf64_phdr_t)) {
        return FINTE_ERR_TYPE;
    }
    uint64_t code_limit = FPROC_STACK_TOP - FPROC_STACK_BYTES;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        uint64_t off = eh->phoff + (uint64_t)i * eh->phentsize;
        if (off + sizeof(elf64_phdr_t) > size) {
            return FINTE_ERR_IO;
        }
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + off);
        if (ph->type != 1 || ph->memsz == 0) {
            continue;
        }
        if (ph->vaddr < FPROC_USER_BASE || ph->vaddr >= code_limit ||
            ph->memsz > code_limit - ph->vaddr ||
            ph->offset + ph->filesz > size) {
            return FINTE_ERR_RANGE;
        }
        uint32_t prot = FPROC_PROT_READ;
        if (ph->flags & 2u) {
            prot |= FPROC_PROT_WRITE;
        }
        if (ph->flags & 1u) {
            prot |= FPROC_PROT_EXEC;
        }
        uint64_t done = 0;
        while (done < ph->memsz) {
            uint64_t va = ph->vaddr + done;
            uint64_t page = va & ~0xFFFull;
            uint64_t pa = finte_as_translate(as, page, 0);
            if (pa == 0) {
                void *fresh = finte_as_alloc_page(as);
                if (!fresh) {
                    return FINTE_ERR_NOMEM;
                }
                pa = (uint64_t)(uintptr_t)fresh;
                if (finte_as_map(as, page, pa, prot) != 0) {
                    finte_as_free_page(as, fresh);
                    return FINTE_ERR_NOMEM;
                }
            }
            uint64_t page_off = va - page;
            uint64_t chunk = 4096u - page_off;
            if (chunk > ph->memsz - done) {
                chunk = ph->memsz - done;
            }
            uint8_t *dst = (uint8_t *)(uintptr_t)(pa + page_off);
            for (uint64_t j = 0; j < chunk; j++) {
                uint64_t src = done + j;
                dst[j] = src < ph->filesz ? image[ph->offset + src] : 0;
            }
            done += chunk;
        }
    }
    if (eh->entry < FPROC_USER_BASE || eh->entry >= code_limit) {
        return FINTE_ERR_RANGE;
    }
    *entry = eh->entry;
    return 0;
}

static int proc_load_shared(faddr_space_t *as, const char *name,
                            uint64_t *entry)
{
    int idx = finte_initrd_find(name);
    if (idx < 0) {
        return FINTE_ERR_NOENT;
    }
    const uint8_t *image = (const uint8_t *)finte_initrd_data((uint32_t)idx);
    uint32_t size = finte_initrd_size((uint32_t)idx);
    if (size < sizeof(elf64_ehdr_t) || image[0] != 0x7F ||
        image[1] != 'E' || image[2] != 'L' || image[3] != 'F' ||
        image[4] != 2 || image[5] != 1) {
        return FINTE_ERR_TYPE;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (eh->machine != as->ops->elf_machine || eh->type != 3 ||
        eh->phentsize < sizeof(elf64_phdr_t)) {
        return FINTE_ERR_TYPE;
    }
    for (uint16_t i = 0; i < eh->phnum; i++) {
        uint64_t off = eh->phoff + (uint64_t)i * eh->phentsize;
        if (off + sizeof(elf64_phdr_t) > size) {
            return FINTE_ERR_IO;
        }
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + off);
        if (ph->type != 1 || ph->memsz == 0) {
            continue;
        }
        if (ph->vaddr < FPROC_USER_BASE ||
            ph->vaddr + ph->memsz > FPROC_USER_LIMIT ||
            ph->offset + ph->filesz > size) {
            return FINTE_ERR_RANGE;
        }
        uint32_t prot = FPROC_PROT_READ;
        if (ph->flags & 2u) {
            prot |= FPROC_PROT_WRITE;
        }
        if (ph->flags & 1u) {
            prot |= FPROC_PROT_EXEC;
        }
        uint64_t done = 0;
        while (done < ph->memsz) {
            uint64_t va = ph->vaddr + done;
            uint64_t page = va & ~0xFFFull;
            uint64_t pa = finte_as_translate(as, page, 0);
            if (pa == 0) {
                void *fresh = finte_as_alloc_page(as);
                if (!fresh) {
                    return FINTE_ERR_NOMEM;
                }
                pa = (uint64_t)(uintptr_t)fresh;
                if (finte_as_map(as, page, pa, prot) != 0) {
                    finte_as_free_page(as, fresh);
                    return FINTE_ERR_NOMEM;
                }
            }
            uint64_t page_off = va - page;
            uint64_t chunk = 4096u - page_off;
            if (chunk > ph->memsz - done) {
                chunk = ph->memsz - done;
            }
            uint8_t *dst = (uint8_t *)(uintptr_t)(pa + page_off);
            for (uint64_t j = 0; j < chunk; j++) {
                uint64_t src = done + j;
                dst[j] = src < ph->filesz ? image[ph->offset + src] : 0;
            }
            done += chunk;
        }
    }
    *entry = eh->entry ? eh->entry : 0x120000000ull;
    return 0;
}

static int proc_map_stack(faddr_space_t *as)
{
    for (uint32_t i = 0; i < FPROC_STACK_PAGES; i++) {
        uint64_t va = FPROC_STACK_TOP - (uint64_t)(i + 1) * 4096u;
        void *page = finte_as_alloc_page(as);
        if (!page) {
            return FINTE_ERR_NOMEM;
        }
        if (finte_as_map(as, va, (uint64_t)(uintptr_t)page,
                         FPROC_PROT_READ | FPROC_PROT_WRITE) != 0) {
            finte_as_free_page(as, page);
            return FINTE_ERR_NOMEM;
        }
    }
    return 0;
}

static int proc_map_stack_into(faddr_space_t *as, uint64_t *stack_top)
{
    int rc = proc_map_stack(as);
    if (rc == 0) {
        *stack_top = FPROC_STACK_TOP;
    }
    return rc;
}

/* Shared open-file descriptions. A descriptor references one of these;
 * dup/fork share the offset, while close drops one reference. The pool is
 * independent of the per-process descriptor table. */
struct ffile_desc {
    uint32_t refs;
    uint8_t used;
    uint8_t kind;
    uint16_t reserved;
    uint32_t flags;
    uint32_t rights;
    uint64_t offset;
    uint64_t size;
    fhandle_t handle;
    const uint8_t *data;
    char path[FDISK_PATH_MAX];
};

static ffile_desc_t g_ofds[FPROC_MAX_OFDS];

static ffile_desc_t *ofd_alloc(uint8_t kind)
{
    uint64_t flags = proc_lock();
    ffile_desc_t *o = NULL;
    for (uint32_t i = 0; i < FPROC_MAX_OFDS; i++) {
        if (g_ofds[i].used) {
            continue;
        }
        o = &g_ofds[i];
        finte_memset(o, 0, sizeof(*o));
        o->used = 1;
        o->refs = 1;
        o->kind = kind;
        o->handle = FINTE_HANDLE_NONE;
        break;
    }
    proc_unlock(flags);
    return o;
}

static void ofd_ref(ffile_desc_t *o)
{
    if (!o) {
        return;
    }
    uint64_t flags = proc_lock();
    if (o->used && o->refs != 0) {
        o->refs++;
    }
    proc_unlock(flags);
}

static void ofd_unref(ffile_desc_t *o)
{
    if (!o) {
        return;
    }
    fhandle_t close_handle = FINTE_HANDLE_NONE;
    uint64_t flags = proc_lock();
    if (!o->used || o->refs == 0) {
        proc_unlock(flags);
        return;
    }
    o->refs--;
    if (o->refs == 0) {
        if (o->kind == FPROC_FD_FILE) {
            close_handle = o->handle;
        }
        o->used = 0;
        o->kind = FPROC_FD_NONE;
        o->handle = FINTE_HANDLE_NONE;
        o->data = NULL;
    }
    proc_unlock(flags);
    if (finte_handle_valid(close_handle)) {
        (void)finte_file_handle_close(close_handle);
    }
}

static ffd_t *fd_at(fproc_t *p, int fd)
{
    if (!p || fd < 0 || (uint32_t)fd >= p->fd_capacity || !p->fds) {
        return NULL;
    }
    return p->fds[fd].ofd ? &p->fds[fd] : NULL;
}

/* Grow the descriptor table geometrically. The table itself is page-backed,
 * so 256 16-byte descriptors fit in one page. */
static int fd_table_grow(fproc_t *p, uint32_t need)
{
    if (!p || need > FPROC_MAX_FDS) {
        return -1;
    }
    if (need <= p->fd_capacity) {
        return 0;
    }
    uint32_t cap = p->fd_capacity ? p->fd_capacity : 16;
    while (cap < need && cap < FPROC_MAX_FDS) {
        cap *= 2;
    }
    if (cap > FPROC_MAX_FDS) {
        cap = FPROC_MAX_FDS;
    }
    if (cap < need) {
        return -1;
    }
    ffd_t *table = (ffd_t *)hal_pages_alloc((size_t)cap * sizeof(ffd_t));
    if (!table) {
        return -1;
    }
    if (p->fds) {
        finte_memcpy(table, p->fds, (size_t)p->fd_capacity * sizeof(ffd_t));
        hal_pages_free(p->fds, (size_t)p->fd_capacity * sizeof(ffd_t));
    }
    p->fds = table;
    p->fd_capacity = cap;
    return 0;
}

static int fd_bind(fproc_t *p, int fd, ffile_desc_t *o, uint8_t fd_flags)
{
    if (!p || !o || fd < 0 || (uint32_t)fd >= p->fd_capacity) {
        return -1;
    }
    p->fds[fd].ofd = o;
    p->fds[fd].fd_flags = fd_flags;
    return 0;
}

static void fd_close(fproc_t *p, int fd);

static int proc_fds_init(fproc_t *p)
{
    if (!p || fd_table_grow(p, 3) != 0) {
        return -1;
    }
    ffile_desc_t *o = ofd_alloc(FPROC_FD_CONSOLE);
    if (!o) {
        return -1;
    }
    ofd_ref(o);
    ofd_ref(o);
    for (int i = 0; i < 3; i++) {
        p->fds[i].ofd = o;
        p->fds[i].fd_flags = 0;
    }
    return 0;
}

static void proc_fds_release(fproc_t *p)
{
    if (!p || !p->fds) {
        return;
    }
    for (uint32_t i = 0; i < p->fd_capacity; i++) {
        if (p->fds[i].ofd) {
            fd_close(p, (int)i);
        }
    }
    hal_pages_free(p->fds, (size_t)p->fd_capacity * sizeof(ffd_t));
    p->fds = NULL;
    p->fd_capacity = 0;
}

static int fd_alloc(fproc_t *p, uint32_t minfd)
{
    if (!p || minfd >= FPROC_MAX_FDS) {
        return -1;
    }
    if (minfd >= p->fd_capacity) {
        if (fd_table_grow(p, minfd + 1) != 0) {
            return -1;
        }
    }
    for (uint32_t i = minfd; i < p->fd_capacity; i++) {
        if (!p->fds[i].ofd) {
            return (int)i;
        }
    }
    uint32_t old = p->fd_capacity;
    if (old >= FPROC_MAX_FDS || fd_table_grow(p, old + 1) != 0) {
        return -1;
    }
    for (uint32_t i = old; i < p->fd_capacity; i++) {
        if (!p->fds[i].ofd) {
            return (int)i;
        }
    }
    return -1;
}

static int fd_dup(fproc_t *p, int oldfd, int newfd, int exact)
{
    ffd_t *src = fd_at(p, oldfd);
    if (!src) {
        return -1;
    }
    if (exact) {
        if (newfd < 0 || newfd >= FPROC_MAX_FDS) {
            return -1;
        }
        if (newfd == oldfd) {
            return oldfd;
        }
        if (fd_table_grow(p, (uint32_t)newfd + 1) != 0) {
            return -1;
        }
        if (p->fds[newfd].ofd) {
            fd_close(p, newfd);
        }
    } else {
        int slot = fd_alloc(p, (uint32_t)newfd);
        if (slot < 0) {
            return -1;
        }
        newfd = slot;
    }
    ofd_ref(src->ofd);
    if (fd_bind(p, newfd, src->ofd, 0) != 0) {
        ofd_unref(src->ofd);
        return -1;
    }
    return newfd;
}

static int sys_fcntl(fproc_t *p, uint64_t fd, uint64_t cmd, uint64_t arg)
{
    ffd_t *e = fd_at(p, (int)fd);
    if (!e) {
        return -1;
    }
    if (cmd == FPROC_F_DUPFD) {
        if ((int32_t)arg < 0) {
            return -1;
        }
        return fd_dup(p, (int)fd, (int)arg, 0);
    }
    if (cmd == FPROC_F_GETFD) {
        return e->fd_flags;
    }
    if (cmd == FPROC_F_SETFD) {
        e->fd_flags = (uint8_t)(arg & FPROC_FD_CLOEXEC);
        return 0;
    }
    if (cmd == FPROC_F_GETFL) {
        return (int)e->ofd->flags;
    }
    if (cmd == FPROC_F_SETFL) {
        e->ofd->flags = (uint32_t)arg;
        return 0;
    }
    return -1;
}

static int fd_open(fproc_t *p, const char *path, uint32_t flags)
{
    if (!p || !path || !*path) {
        return -1;
    }
    int fd = fd_alloc(p, 0);
    if (fd < 0) {
        return -1;
    }

    const char *name = path;
    while (*name == '/') {
        name++;
    }
    int idx = finte_initrd_find(name);
    if (idx >= 0 && !(flags & FPROC_OPEN_WRITE)) {
        ffile_desc_t *o = ofd_alloc(FPROC_FD_INITRD);
        if (!o) {
            return -1;
        }
        o->flags = flags;
        o->data = (const uint8_t *)finte_initrd_data((uint32_t)idx);
        o->size = finte_initrd_size((uint32_t)idx);
        finte_strlcpy(o->path, path, sizeof(o->path));
        if (fd_bind(p, fd, o, 0) != 0) {
            ofd_unref(o);
            return -1;
        }
        return fd;
    }

    uint32_t oflags = FINTE_OPEN_READ;
    if (flags & FPROC_OPEN_WRITE) {
        oflags = FINTE_OPEN_WRITE;
    }
    if (flags & FPROC_OPEN_CREATE) {
        oflags |= FINTE_OPEN_CREATE;
    }
    if (flags & FPROC_OPEN_TRUNC) {
        oflags |= FINTE_OPEN_TRUNC;
    }
    fhandle_t h = FINTE_HANDLE_NONE;
    int rc = finte_disk_file_open(path, oflags, &h);
    if (rc != 0) {
        return -1;
    }
    uint32_t rights = (flags & FPROC_OPEN_WRITE) ? FINTE_R_WRITE : FINTE_R_READ;
    if (flags & FPROC_OPEN_WRITE) {
        rights |= FINTE_R_READ;
    }
    ffile_desc_t *o = ofd_alloc(FPROC_FD_FILE);
    if (!o) {
        (void)finte_file_handle_close(h);
        return -1;
    }
    o->flags = flags;
    o->rights = rights;
    o->handle = h;
    int64_t sz = finte_file_handle_size(h);
    o->size = sz > 0 ? (uint64_t)sz : 0;
    finte_strlcpy(o->path, path, sizeof(o->path));
    if (fd_bind(p, fd, o, 0) != 0) {
        ofd_unref(o);
        return -1;
    }
    return fd;
}

static void fd_close(fproc_t *p, int fd)
{
    if (!p || fd < 0 || (uint32_t)fd >= p->fd_capacity || !p->fds) {
        return;
    }
    ffd_t *e = &p->fds[fd];
    ffile_desc_t *o = e->ofd;
    if (!o) {
        return;
    }
    e->ofd = NULL;
    e->fd_flags = 0;
    ofd_unref(o);
}

static int fd_read_at(fproc_t *p, ffd_t *e, uint64_t offset, void *dst,
                      size_t len)
{
    (void)p;
    ffile_desc_t *o = e ? e->ofd : NULL;
    if (!o) {
        return -1;
    }
    if (o->kind == FPROC_FD_INITRD) {
        if (offset >= o->size) {
            return 0;
        }
        size_t n = (size_t)(o->size - offset);
        if (n > len) {
            n = len;
        }
        finte_memcpy(dst, o->data + offset, n);
        return (int)n;
    }
    if (o->kind == FPROC_FD_FILE) {
        uint8_t bounce[256];
        size_t total = 0;
        while (total < len) {
            size_t chunk = len - total;
            if (chunk > sizeof(bounce)) {
                chunk = sizeof(bounce);
            }
            int64_t r = finte_file_handle_read_at(o->handle, offset + total,
                                                  bounce, chunk);
            if (r < 0) {
                return total ? (int)total : -1;
            }
            if (r == 0) {
                break;
            }
            finte_memcpy((uint8_t *)dst + total, bounce, (size_t)r);
            total += (size_t)r;
            if ((size_t)r < chunk) {
                break;
            }
        }
        return (int)total;
    }
    return -1;
}

static int fd_read(fproc_t *p, int fd, void *dst, size_t len)
{
    ffd_t *e = fd_at(p, fd);
    if (!e) {
        return -1;
    }
    int n = fd_read_at(p, e, e->ofd->offset, dst, len);
    if (n > 0) {
        e->ofd->offset += (uint64_t)n;
    }
    return n;
}

static int fd_pread(fproc_t *p, int fd, uint64_t offset, void *dst, size_t len)
{
    ffd_t *e = fd_at(p, fd);
    if (!e) {
        return -1;
    }
    return fd_read_at(p, e, offset, dst, len);
}

static int fd_write(fproc_t *p, int fd, const void *src, size_t len)
{
    ffd_t *e = fd_at(p, fd);
    if (!e) {
        return -1;
    }
    ffile_desc_t *o = e->ofd;
    if (o->kind == FPROC_FD_CONSOLE) {
        hal_console_write((const char *)src, len);
        return (int)len;
    }
    if (o->kind == FPROC_FD_FILE) {
        uint8_t bounce[256];
        size_t total = 0;
        while (total < len) {
            size_t chunk = len - total;
            if (chunk > sizeof(bounce)) {
                chunk = sizeof(bounce);
            }
            finte_memcpy(bounce, (const uint8_t *)src + total, chunk);
            int64_t r = finte_file_handle_write_at(
                o->handle, o->offset + total, bounce, chunk);
            if (r != (int64_t)chunk) {
                return total ? (int)total : -1;
            }
            total += chunk;
        }
        o->offset += total;
        if (o->offset > o->size) {
            o->size = o->offset;
        }
        return (int)total;
    }
    return -1;
}

static int proc_copy_in(faddr_space_t *as, void *dst, uint64_t uva, size_t len)
{
    if (!finte_as_user_ok(as, uva, len, 0)) {
        return -1;
    }
    while (len) {
        uint64_t page = uva & ~0xFFFull;
        uint64_t pa = finte_as_translate(as, page, 0);
        size_t off = (size_t)(uva - page);
        size_t chunk = 4096u - off;
        if (chunk > len) {
            chunk = len;
        }
        finte_memcpy(dst, (const void *)(uintptr_t)(pa + off), chunk);
        dst = (uint8_t *)dst + chunk;
        uva += chunk;
        len -= chunk;
    }
    return 0;
}

static int proc_copy_out(faddr_space_t *as, uint64_t uva, const void *src,
                         size_t len)
{
    if (!finte_as_user_ok(as, uva, len, 1)) {
        return -1;
    }
    while (len) {
        uint64_t page = uva & ~0xFFFull;
        uint64_t pa = finte_as_translate(as, page, 1);
        if (!pa) {
            if (finte_as_cow_fault(as, page, 1) != 0) {
                return -1;
            }
            pa = finte_as_translate(as, page, 1);
            if (!pa) {
                return -1;
            }
        }
        size_t off = (size_t)(uva - page);
        size_t chunk = 4096u - off;
        if (chunk > len) {
            chunk = len;
        }
        finte_memcpy((void *)(uintptr_t)(pa + off), src, chunk);
        src = (const uint8_t *)src + chunk;
        uva += chunk;
        len -= chunk;
    }
    return 0;
}

int finte_user_copy_in(fflow_t *f, void *dst, uint64_t uva, size_t len)
{
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
    return proc_copy_in(p ? p->space : NULL, dst, uva, len);
}

int finte_user_copy_out(fflow_t *f, uint64_t uva, const void *src, size_t len)
{
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
    return proc_copy_out(p ? p->space : NULL, uva, src, len);
}

static void user_process_entry(fflow_t *f)
{
    fproc_t *p = f ? (fproc_t *)f->user : NULL;
    if (!p || !f->thread || !f->thread->ctx) {
        finte_flow_exit(f, 1);
    }
    fhal_ctx_t *ctx = f->thread->ctx;
    ctx->addr_space = finte_as_root(p->space);
    finte_as_switch(p->space);
    uint64_t kstack_top = (uint64_t)(uintptr_t)ctx->stack + ctx->stack_size;
    fproc_arch()->enter_user(p->entry, p->stack_top - 16u, kstack_top);
}

static const fprogram_t g_user_program = {
    "user-process", user_process_entry, 1u << 20
};

static void proc_init_maps(fproc_t *p)
{
    finte_memset(p->maps, 0, sizeof(p->maps));
    p->mmap_next = FPROC_MMAP_BASE;
}

static void proc_free_aux(fproc_t *p)
{
    if (p->fork_frame) {
        hal_pages_free(p->fork_frame, 4096);
        p->fork_frame = NULL;
    }
    if (p->sig_frame) {
        hal_pages_free(p->sig_frame, 4096);
        p->sig_frame = NULL;
    }
}

fproc_t *finte_proc_create(const char *name, const char *elf, uint32_t pid,
                           uint64_t shared_phys, fhandle_t chan,
                           uint32_t chan_rights, fhandle_t arena,
                           uint32_t arena_page, uint32_t arena_rights)
{
    fproc_t *p = proc_alloc();
    if (!p) {
        return NULL;
    }
    if (pid == 0) {
        pid = proc_alloc_pid();
    }
    if (pid == 0 || finte_proc_find(pid)) {
        hal_pages_free(p, PROC_BYTES);
        return NULL;
    }
    p->pid = pid;
    p->pgid = pid;
    p->shared_phys = shared_phys;
    p->ipc_cap = -1;
    p->arena_cap = -1;
    finte_strlcpy(p->name, name, sizeof(p->name));
    p->space = finte_as_create();
    if (!p->space) {
        proc_free_aux(p);
        hal_pages_free(p, PROC_BYTES);
        return NULL;
    }
    proc_init_maps(p);
    p->sig_frame = hal_pages_alloc(4096);
    if (!p->sig_frame) {
        finte_as_unref(p->space);
        p->space = NULL;
        proc_free_aux(p);
        hal_pages_free(p, PROC_BYTES);
        return NULL;
    }
    finte_memset(p->sig_frame, 0, 4096);
    if (proc_load_elf(p->space, elf, &p->entry) != 0 ||
        proc_map_stack_into(p->space, &p->stack_top) != 0) {
        finte_as_unref(p->space);
        proc_free_aux(p);
        hal_pages_free(p, PROC_BYTES);
        return NULL;
    }
    if (shared_phys) {
        if (finte_as_map(p->space, FPROC_SHARED_VA, shared_phys,
                         FPROC_PROT_READ | FPROC_PROT_WRITE) != 0) {
            finte_as_unref(p->space);
            proc_free_aux(p);
            hal_pages_free(p, PROC_BYTES);
            return NULL;
        }
    }
    p->flow_handle = finte_flow_create(name, &g_user_program,
                                       1u << 20, FINTE_PERSONA_NATIVE);
    if (!finte_handle_valid(p->flow_handle)) {
        finte_as_unref(p->space);
        proc_free_aux(p);
        hal_pages_free(p, PROC_BYTES);
        return NULL;
    }
    p->flow = finte_flow_lookup(p->flow_handle);
    if (!p->flow) {
        finte_node_unref(p->flow_handle);
        p->flow_handle = FINTE_HANDLE_NONE;
        finte_as_unref(p->space);
        proc_free_aux(p);
        hal_pages_free(p, PROC_BYTES);
        return NULL;
    }
    p->flow->user = p;
    p->cpu = p->flow->thread ? p->flow->thread->cpu : 0;
    if (p->flow->thread) {
        p->flow->thread->priority = 0;
    }
    if (finte_handle_valid(chan) && chan_rights) {
        int slot = finte_cap_install(p->flow, chan, chan_rights);
        if (slot < 0) {
            finte_proc_destroy(p);
            return NULL;
        }
        p->ipc_cap = slot;
    }
    if (finte_handle_valid(arena) && arena_rights) {
        int slot = finte_cap_install_arena_pages(p->flow, arena, arena_page, 1,
                                                 arena_rights);
        if (slot < 0) {
            finte_proc_destroy(p);
            return NULL;
        }
        p->arena_cap = slot;
    }
    if (proc_fds_init(p) != 0) {
        finte_proc_destroy(p);
        return NULL;
    }
    proc_table_add(p);
    return p;
}

int finte_proc_start(fproc_t *p)
{
    if (!p || !p->space || !p->flow) {
        return FINTE_ERR_STATE;
    }
    if (!p->flow->space) {
        /* Transfer the creation reference to the Flow. This is the only
         * owner of the address space; p->space is a weak pointer. */
        p->flow->space = p->space;
    }
    if (p->flow->thread && p->flow->thread->ctx) {
        p->flow->thread->ctx->addr_space = finte_as_root(p->space);
    }
    int rc = finte_flow_start(p->flow_handle).status;
    if (rc != 0 && p->flow->space == p->space) {
        finte_as_unref(p->flow->space);
        p->flow->space = NULL;
    }
    return rc;
}

void finte_proc_destroy(fproc_t *p)
{
    if (!p) {
        return;
    }
    p->parent = NULL;
    if (p->flow && p->flow->started && p->flow->state != FINTE_FLOW_DEAD &&
        p->flow->state != FINTE_FLOW_FREE) {
        /* The live flow still owns this process object; abort it and leave
         * teardown to flow death rather than freeing a live address space. */
        finte_proc_abort(p);
        return;
    }
    while (p->children) {
        fproc_t *child = p->children;
        p->children = child->next_sibling;
        child->next_sibling = NULL;
        child->parent = NULL;
        if (child->flow && child->flow->state != FINTE_FLOW_DEAD &&
            child->flow->state != FINTE_FLOW_FREE) {
            finte_proc_abort(child);
            continue; /* the live flow still owns this process object */
        }
        finte_proc_destroy(child);
    }
    proc_fds_release(p);
    proc_table_remove(p);
    finte_service_owner_exit(p->pid);
    if (p->flow && p->flow->space == p->space) {
        if (!p->flow->started) {
            finte_as_unref(p->flow->space);
            p->flow->space = NULL;
        }
    } else if (p->space) {
        finte_as_unref(p->space);
    }
    p->space = NULL;
    if (finte_handle_valid(p->flow_handle)) {
        if (p->flow) {
            finte_cap_drop_all(p->flow);
        }
        finte_node_unref(p->flow_handle);
    }
    p->flow_handle = FINTE_HANDLE_NONE;
    p->flow = NULL;
    if (p->sig_frame && p->sig_frame != p->fork_frame) {
        hal_pages_free(p->sig_frame, 4096);
    }
    p->sig_frame = NULL;
    if (p->fork_frame) {
        hal_pages_free(p->fork_frame, 4096);
    }
    p->fork_frame = NULL;
    hal_pages_free(p, PROC_BYTES);
}

void finte_user_fault(uint64_t vec, uint64_t rip, uint64_t cr2)
{
    fflow_t *f = finte_flow_current();
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
    if (!p) {
        return;
    }
    char buf[192];
    int n = finte_format(buf, sizeof(buf),
                         "\n[user] pid %u killed: exception %llu "
                         "pc=0x%llx addr=0x%llx\n",
                         (unsigned)p->pid, (unsigned long long)vec,
                         (unsigned long long)rip,
                         (unsigned long long)cr2);
    hal_console_write(buf, (size_t)n);
    p->faulted = 1;
    if (!p->expected_fault) {
        char reason[64];
        int rn = finte_format(reason, sizeof(reason),
                              "user fault pid %u vec %llu",
                              (unsigned)p->pid, (unsigned long long)vec);
        if (rn > 0) {
            hal_watchdog_strike(reason);
        }
    }
    finte_flow_exit(f, 128 + (int)vec);
}

static void proc_timer_poll(fproc_t *p);

/* Run pending signals on the way back to user mode. The arch frame remains
 * owned by the trap handler; this only mutates it. */
void finte_process_signal_check(fproc_t *p, void *frame)
{
    if (p) {
        proc_timer_poll(p);
    }
    if (!p || !frame || p->sig_frame == NULL) {
        return;
    }
    for (;;) {
        if (p->killed) {
            p->killed = 0;
            finte_flow_exit(p->flow, 128 + FPROC_SIGKILL);
        }
        if (!p->stopped) {
            break;
        }
        finte_flow_stop(p->flow);
    }
    const fproc_arch_ops_t *ops = fproc_arch();
    if (!ops->frame_size || !ops->signal_setup) {
        return;
    }
    for (uint32_t sig = 1; sig < FPROC_MAX_SIGNALS; sig++) {
        if (!p->sigs[sig].pending) {
            continue;
        }
        if (p->sig_blocked & (1u << sig)) {
            continue;
        }
        if (p->sigs[sig].handler) {
            p->sigs[sig].pending = 0;
            finte_memcpy(p->sig_frame, frame, ops->frame_size());
            if (ops->signal_setup(p->space, frame, p->sigs[sig].handler,
                                  p->sigs[sig].restorer, sig) == 0) {
                return;
            }
            p->sigs[sig].pending = 1;
            continue;
        }
        /* Default action: SIGCHLD is ignored, the rest terminate. */
        p->sigs[sig].pending = 0;
        if (sig == FPROC_SIGCHLD) {
            continue;
        }
        finte_flow_exit(p->flow, 128 + (int)sig);
    }
}

static int fd_read_syscall(fproc_t *p, uint64_t fd, uint64_t uva, uint64_t len)
{
    if (!p || fd >= FPROC_MAX_FDS || len > 1024u * 1024u) {
        return -1;
    }
    uint8_t bounce[256];
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }
        int n = fd_read(p, (int)fd, bounce, chunk);
        if (n < 0) {
            return total ? (int)total : -1;
        }
        if (n == 0) {
            break;
        }
        if (proc_copy_out(p->space, uva + total, bounce, (size_t)n) != 0) {
            return -1;
        }
        total += (size_t)n;
        if ((size_t)n < chunk) {
            break;
        }
    }
    return (int)total;
}

static int fd_write_syscall(fproc_t *p, uint64_t fd, uint64_t uva, uint64_t len)
{
    if (!p || fd >= FPROC_MAX_FDS || len > 1024u * 1024u) {
        return -1;
    }
    uint8_t bounce[256];
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }
        if (proc_copy_in(p->space, bounce, uva + total, chunk) != 0) {
            return -1;
        }
        int n = fd_write(p, (int)fd, bounce, chunk);
        if (n <= 0) {
            return total ? (int)total : -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}

static uint64_t sys_mmap(fproc_t *p, uint64_t addr, uint64_t len, uint64_t prot)
{
    if (!p || len == 0) {
        return (uint64_t)-1;
    }
    if (prot == 0) {
        prot = FPROC_PROT_READ | FPROC_PROT_WRITE;
    }
    uint64_t pages64 = (len + 4095u) / 4096u;
    if (pages64 == 0 || pages64 > FPROC_MAX_MAP_PAGES) {
        return (uint64_t)-1;
    }
    uint32_t pages = (uint32_t)pages64;
    uint64_t va;
    int slot = -1;
    for (int i = 0; i < FPROC_MAX_MAPS; i++) {
        if (!p->maps[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return (uint64_t)-1;
    }
    if (addr) {
        va = addr & ~0xFFFull;
    } else {
        va = (p->mmap_next + 0xFFFu) & ~0xFFFull;
    }
    uint64_t end = va + pages * 4096u;
    if (end < va || va < FPROC_MMAP_BASE || end > FPROC_MMAP_LIMIT) {
        return (uint64_t)-1;
    }
    for (uint32_t i = 0; i < pages; i++) {
        void *page = finte_as_alloc_page(p->space);
        if (!page) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t pa = 0;
                if (finte_as_unmap(p->space, va + j * 4096u, &pa) == 0 &&
                    pa) {
                    finte_as_free_page(p->space, (void *)(uintptr_t)pa);
                }
            }
            return (uint64_t)-1;
        }
        if (finte_as_map(p->space, va + i * 4096u,
                         (uint64_t)(uintptr_t)page, (uint32_t)prot) != 0) {
            finte_as_free_page(p->space, page);
            for (uint32_t j = 0; j < i; j++) {
                uint64_t pa = 0;
                if (finte_as_unmap(p->space, va + j * 4096u, &pa) == 0 &&
                    pa) {
                    finte_as_free_page(p->space, (void *)(uintptr_t)pa);
                }
            }
            return (uint64_t)-1;
        }
    }
    p->maps[slot].used = 1;
    p->maps[slot].va = va;
    p->maps[slot].len = pages * 4096u;
    p->maps[slot].prot = (uint8_t)prot;
    p->maps[slot].page_count = (uint16_t)pages;
    if (!addr) {
        p->mmap_next = end;
    }
    return va;
}

static uint64_t sys_mmap_file(fproc_t *p, uint64_t fd, uint64_t len,
                              uint64_t prot)
{
    if (!p || !p->space || fd >= FPROC_MAX_FDS || len == 0) {
        return (uint64_t)-1;
    }
    ffd_t *e = fd_at(p, (int)fd);
    if (!e || e->ofd->kind == FPROC_FD_CONSOLE) {
        return (uint64_t)-1;
    }
    if (prot == 0) {
        prot = FPROC_PROT_READ | FPROC_PROT_WRITE;
    }
    if (!(prot & FPROC_PROT_READ)) {
        return (uint64_t)-1;
    }
    uint64_t pages64 = (len + 4095u) / 4096u;
    if (pages64 == 0 || pages64 > FPROC_MAX_MAP_PAGES) {
        return (uint64_t)-1;
    }
    uint32_t pages = (uint32_t)pages64;
    int slot = -1;
    for (int i = 0; i < FPROC_MAX_MAPS; i++) {
        if (!p->maps[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return (uint64_t)-1;
    }
    uint64_t va = (p->mmap_next + 0xFFFu) & ~0xFFFull;
    uint64_t end = va + (uint64_t)pages * 4096u;
    if (end < va || va < FPROC_MMAP_BASE || end > FPROC_MMAP_LIMIT) {
        return (uint64_t)-1;
    }
    for (uint32_t i = 0; i < pages; i++) {
        void *page = finte_as_alloc_page(p->space);
        if (!page) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t pa = 0;
                if (finte_as_unmap(p->space, va + j * 4096u, &pa) == 0 &&
                    pa) {
                    finte_as_free_page(p->space, (void *)(uintptr_t)pa);
                }
            }
            return (uint64_t)-1;
        }
        int n = fd_pread(p, (int)fd, (uint64_t)i * 4096u, page, 4096);
        if (n < 0) {
            finte_as_free_page(p->space, page);
            for (uint32_t j = 0; j < i; j++) {
                uint64_t pa = 0;
                if (finte_as_unmap(p->space, va + j * 4096u, &pa) == 0 &&
                    pa) {
                    finte_as_free_page(p->space, (void *)(uintptr_t)pa);
                }
            }
            return (uint64_t)-1;
        }
        if (finte_as_map(p->space, va + i * 4096u,
                         (uint64_t)(uintptr_t)page, (uint32_t)prot) != 0) {
            finte_as_free_page(p->space, page);
            for (uint32_t j = 0; j < i; j++) {
                uint64_t pa = 0;
                if (finte_as_unmap(p->space, va + j * 4096u, &pa) == 0 &&
                    pa) {
                    finte_as_free_page(p->space, (void *)(uintptr_t)pa);
                }
            }
            return (uint64_t)-1;
        }
    }
    p->maps[slot].used = 1;
    p->maps[slot].va = va;
    p->maps[slot].len = (uint64_t)pages * 4096u;
    p->maps[slot].prot = (uint8_t)prot;
    p->maps[slot].page_count = (uint16_t)pages;
    p->mmap_next = end;
    return va;
}

static int sys_munmap(fproc_t *p, uint64_t va, uint64_t len)
{
    if (!p || len == 0) {
        return -1;
    }
    for (int i = 0; i < FPROC_MAX_MAPS; i++) {
        if (!p->maps[i].used) {
            continue;
        }
        if (va < p->maps[i].va || va + len > p->maps[i].va + p->maps[i].len) {
            continue;
        }
        for (uint32_t j = 0; j < p->maps[i].page_count; j++) {
            uint64_t page_va = p->maps[i].va + j * 4096u;
            if (page_va < va || page_va >= va + len) {
                continue;
            }
            uint64_t pa = 0;
            if (finte_as_unmap(p->space, page_va, &pa) == 0 && pa) {
                finte_as_free_page(p->space, (void *)(uintptr_t)pa);
            }
        }
        p->maps[i].used = 0;
        return 0;
    }
    return -1;
}

static void proc_deliver_signal(fproc_t *target, uint64_t sig)
{
    if (!target) {
        return;
    }
    finte_core_lock();
    if (sig == FPROC_SIGSTOP) {
        if (!target->stopped) {
            target->stopped = 1;
            target->stop_acked = 0;
            target->stop_pending = 1;
        }
    } else if (sig == FPROC_SIGCONT) {
        if (target->stopped) {
            target->stopped = 0;
            target->stop_acked = 0;
            target->cont_pending = 1;
        }
    } else if (sig == FPROC_SIGKILL) {
        target->killed = 1;
        target->stopped = 0;
        target->stop_acked = 0;
    } else {
        target->sigs[sig].pending = 1;
    }
    finte_core_unlock();
    if (target->flow && target->flow->thread) {
        finte_sched_wake_thread(target->flow->thread);
    }
}

static int sys_kill(fproc_t *p, uint64_t pid, uint64_t sig)
{
    if (!p || sig >= FPROC_MAX_SIGNALS) {
        return -1;
    }
    int32_t target = (int32_t)pid;
    if (target <= 0) {
        uint32_t pgid = (target == 0) ? p->pgid
                                     : (uint32_t)(-(int64_t)target);
        fproc_t *members[FPROC_MAX_PROCS];
        int n = proc_collect_pgid(pgid, members, FPROC_MAX_PROCS);
        if (n <= 0) {
            return -1;
        }
        if (sig == 0) {
            return 0;
        }
        for (int i = 0; i < n; i++) {
            proc_deliver_signal(members[i], sig);
        }
        return 0;
    }
    fproc_t *t = finte_proc_find((uint32_t)target);
    if (!t) {
        return -1;
    }
    if (sig == 0) {
        return 0;
    }
    proc_deliver_signal(t, sig);
    return 0;
}

static int sys_setpgid(fproc_t *p, uint64_t pid, uint64_t pgid)
{
    if (!p) {
        return -1;
    }
    int32_t tpid = (int32_t)pid;
    if (tpid < 0 || (int32_t)pgid < 0) {
        return -1;
    }
    fproc_t *target = (tpid == 0) ? p : finte_proc_find((uint32_t)tpid);
    if (!target || (target != p && target->parent != p)) {
        return -1;
    }
    uint32_t group = (pgid == 0) ? target->pid : (uint32_t)pgid;
    if (group == 0) {
        return -1;
    }
    if (group != target->pid && !proc_pgid_exists(group)) {
        return -1;
    }
    finte_core_lock();
    target->pgid = group;
    finte_core_unlock();
    return 0;
}

static uint64_t sys_getpgrp(fproc_t *p)
{
    return p ? (uint64_t)p->pgid : 0;
}

/* POSIX interval timers, ITIMER_REAL only. The scheduler tick calls
 * finte_proc_timer_tick(), which arms SIGALRM; handlers and masks are the
 * normal signal path. */
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} proc_timeval_t;

typedef struct {
    proc_timeval_t interval;
    proc_timeval_t value;
} proc_itimerval_t;

static int proc_timeval_ns(const proc_timeval_t *tv, uint64_t *out)
{
    if (!tv || !out || tv->tv_sec < 0 || tv->tv_usec < 0 ||
        tv->tv_usec >= 1000000) {
        return -1;
    }
    uint64_t sec = (uint64_t)tv->tv_sec;
    if (sec > (~0ull - 999999ull * 1000ull) / 1000000000ull) {
        return -1;
    }
    *out = sec * 1000000000ull + (uint64_t)tv->tv_usec * 1000ull;
    return 0;
}

static void proc_timer_snapshot(fproc_t *p, proc_itimerval_t *out)
{
    finte_memset(out, 0, sizeof(*out));
    uint64_t now = hal_now_ns();
    uint64_t flags = proc_lock();
    uint64_t expire = p ? p->timer_expire_ns : 0;
    uint64_t interval = p ? p->timer_interval_ns : 0;
    proc_unlock(flags);
    if (expire) {
        uint64_t delta = expire > now ? expire - now : 0;
        uint64_t us = (delta + 999ull) / 1000ull;
        out->value.tv_sec = (int64_t)(us / 1000000ull);
        out->value.tv_usec = (int64_t)(us % 1000000ull);
    }
    out->interval.tv_sec = (int64_t)(interval / 1000000000ull);
    out->interval.tv_usec = (int64_t)((interval % 1000000000ull) / 1000ull);
}

static int sys_getitimer(fproc_t *p, uint64_t which, uint64_t old_uva)
{
    if (!p || !p->space || which != 0 || old_uva == 0) {
        return -1;
    }
    proc_itimerval_t old;
    proc_timer_snapshot(p, &old);
    return proc_copy_out(p->space, old_uva, &old, sizeof(old));
}

static int sys_setitimer(fproc_t *p, uint64_t which, uint64_t new_uva,
                         uint64_t old_uva)
{
    if (!p || !p->space || which != 0) {
        return -1;
    }
    proc_itimerval_t nv;
    uint64_t value = 0;
    uint64_t interval = 0;
    if (new_uva != 0) {
        if (proc_copy_in(p->space, &nv, new_uva, sizeof(nv)) != 0 ||
            proc_timeval_ns(&nv.value, &value) != 0 ||
            proc_timeval_ns(&nv.interval, &interval) != 0) {
            return -1;
        }
    }
    if (old_uva != 0) {
        proc_itimerval_t old;
        proc_timer_snapshot(p, &old);
        if (proc_copy_out(p->space, old_uva, &old, sizeof(old)) != 0) {
            return -1;
        }
    }
    uint64_t now = hal_now_ns();
    if (value > ~0ull - now) {
        value = ~0ull - now;
    }
    uint64_t flags = proc_lock();
    if (value) {
        p->timer_expire_ns = now + value;
        p->timer_interval_ns = interval;
    } else {
        p->timer_expire_ns = 0;
        p->timer_interval_ns = 0;
    }
    proc_unlock(flags);
    return 0;
}

static uint64_t sys_alarm(fproc_t *p, uint64_t seconds)
{
    if (!p) {
        return 0;
    }
    uint32_t secs = (uint32_t)seconds;
    uint64_t now = hal_now_ns();
    uint64_t flags = proc_lock();
    uint64_t remain = 0;
    if (p->timer_expire_ns) {
        uint64_t delta =
            p->timer_expire_ns > now ? p->timer_expire_ns - now : 0;
        remain = (delta + 999999999ull) / 1000000000ull;
    }
    if (secs) {
        p->timer_expire_ns = now + (uint64_t)secs * 1000000000ull;
        p->timer_interval_ns = 0;
    } else {
        p->timer_expire_ns = 0;
        p->timer_interval_ns = 0;
    }
    proc_unlock(flags);
    return remain;
}

static void proc_timer_expire_locked(fproc_t *p, uint64_t now)
{
    if (!p || !p->timer_expire_ns || now < p->timer_expire_ns) {
        return;
    }
    p->sigs[FPROC_SIGALRM].pending = 1;
    if (p->timer_interval_ns) {
        p->timer_expire_ns = now + p->timer_interval_ns;
    } else {
        p->timer_expire_ns = 0;
    }
}

static void proc_timer_poll(fproc_t *p)
{
    if (!p) {
        return;
    }
    uint64_t now = hal_now_ns();
    uint64_t flags = proc_lock();
    proc_timer_expire_locked(p, now);
    proc_unlock(flags);
}

void finte_proc_timer_tick(void)
{
    uint64_t now = hal_now_ns();
    fproc_t *fired[FPROC_MAX_PROCS];
    uint32_t nfired = 0;
    uint64_t flags = proc_lock();
    for (uint32_t i = 0; i < FPROC_MAX_PROCS; i++) {
        fproc_t *p = g_procs[i];
        if (!p || !p->timer_expire_ns || now < p->timer_expire_ns) {
            continue;
        }
        proc_timer_expire_locked(p, now);
        fired[nfired++] = p;
    }
    proc_unlock(flags);
    for (uint32_t i = 0; i < nfired; i++) {
        if (fired[i]->flow && fired[i]->flow->thread) {
            finte_sched_wake_thread(fired[i]->flow->thread);
        }
    }
}

static uint64_t sys_sigprocmask(fproc_t *p, uint64_t how, uint64_t mask)
{
    if (!p) {
        return (uint64_t)-1;
    }
    uint32_t bits = (uint32_t)mask & ~(1u << FPROC_SIGKILL);
    uint32_t old = p->sig_blocked;
    if (how == FPROC_SIG_BLOCK) {
        bits = old | bits;
    } else if (how == FPROC_SIG_UNBLOCK) {
        bits = old & ~bits;
    } else if (how != FPROC_SIG_SETMASK) {
        return (uint64_t)-1;
    }
    p->sig_blocked = bits;
    return (uint64_t)old;
}

static int sys_signal(fproc_t *p, uint64_t sig, uint64_t handler,
                      uint64_t restorer)
{
    if (!p || sig >= FPROC_MAX_SIGNALS || sig == FPROC_SIGKILL ||
        sig == FPROC_SIGSTOP) {
        return -1;
    }
    p->sigs[sig].handler = handler;
    p->sigs[sig].restorer = restorer;
    p->sigs[sig].pending = 0;
    return 0;
}

uint64_t finte_proc_syscall(fproc_t *p, uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2, void *frame)
{
    fflow_t *f = finte_flow_current();
    if (!p) {
        p = (f && f->user) ? (fproc_t *)f->user : NULL;
    }
    if (nr == FPROC_SYS_WRITE_FD) {
        return (uint64_t)fd_write_syscall(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_DYNSVC) {
        if (!p || a1 == 0 || a1 >= FINTE_NAME_MAX) {
            return (uint64_t)-1;
        }
        char name[FINTE_NAME_MAX];
        if (proc_copy_in(p->space, name, a0, (size_t)a1) != 0) {
            return (uint64_t)-1;
        }
        name[a1] = 0;
        uint64_t entry = 0;
        if (proc_load_shared(p->space, name, &entry) != 0) {
            return (uint64_t)-1;
        }
        return entry;
    }
    if (nr == FPROC_SYS_SERVICE_REG || nr == FPROC_SYS_SERVICE_LOOK ||
        nr == FPROC_SYS_SERVICE_UNREG) {
        if (!p || a1 == 0 || a1 >= FINTE_NAME_MAX) {
            return (uint64_t)-1;
        }
        char name[FINTE_NAME_MAX];
        if (proc_copy_in(p->space, name, a0, (size_t)a1) != 0) {
            return (uint64_t)-1;
        }
        name[a1] = 0;
        if (nr == FPROC_SYS_SERVICE_REG) {
            return (uint64_t)finte_service_register(p, name, (uint16_t)a2);
        }
        if (nr == FPROC_SYS_SERVICE_LOOK) {
            int slot = finte_service_lookup(p, name);
            return slot < 0 ? (uint64_t)-1 : (uint64_t)slot;
        }
        return (uint64_t)finte_service_unregister(p, name);
    }
    if (nr == FPROC_SYS_WRITE) {
        if (a1 > 1024u * 1024u) {
            return (uint64_t)-1;
        }
        char buf[256];
        uint64_t left = a1;
        uint64_t at = a0;
        while (left) {
            size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            if (finte_user_copy_in(f, buf, at, chunk) != 0) {
                return (uint64_t)-1;
            }
            hal_console_write(buf, chunk);
            at += chunk;
            left -= chunk;
        }
        return a1;
    }
    if (nr == FPROC_SYS_READ) {
        return (uint64_t)fd_read_syscall(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_OPEN) {
        if (!p || a2 > 4096u) {
            return (uint64_t)-1;
        }
        char path[FDISK_PATH_MAX];
        size_t n = (size_t)a2;
        if (n == 0 || n >= sizeof(path) ||
            finte_user_copy_in(f, path, a0, n) != 0) {
            return (uint64_t)-1;
        }
        path[n] = 0;
        return (uint64_t)fd_open(p, path, (uint32_t)a1);
    }
    if (nr == FPROC_SYS_CLOSE) {
        fd_close(p, (int)a0);
        return 0;
    }
    if (nr == FPROC_SYS_DUP) {
        return (uint64_t)fd_dup(p, (int)a0, 0, 0);
    }
    if (nr == FPROC_SYS_DUP2) {
        return (uint64_t)fd_dup(p, (int)a0, (int)a1, 1);
    }
    if (nr == FPROC_SYS_FCNTL) {
        return (uint64_t)sys_fcntl(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_LSEEK) {
        ffd_t *e = fd_at(p, (int)a0);
        if (!e) {
            return (uint64_t)-1;
        }
        ffile_desc_t *o = e->ofd;
        if (o->kind != FPROC_FD_INITRD && o->kind != FPROC_FD_FILE) {
            return (uint64_t)-1;
        }
        int64_t base = 0;
        if (a2 == 1) {
            base = (int64_t)o->offset;
        } else if (a2 == 2) {
            base = (int64_t)o->size;
        } else if (a2 != 0) {
            return (uint64_t)-1;
        }
        int64_t pos = base + (int64_t)a1;
        if (pos < 0) {
            return (uint64_t)-1;
        }
        o->offset = (uint64_t)pos;
        return (uint64_t)pos;
    }
    if (nr == FPROC_SYS_MMAP) {
        return sys_mmap(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_MMAP_FILE) {
        return sys_mmap_file(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_MUNMAP) {
        return (uint64_t)sys_munmap(p, a0, a1);
    }
    if (nr == FPROC_SYS_FORK) {
        extern uint64_t finte_proc_fork(fproc_t *parent, void *frame);
        return finte_proc_fork(p, frame);
    }
    if (nr == FPROC_SYS_EXEC) {
        extern uint64_t finte_proc_exec(fproc_t *p, uint64_t path_uva,
                                        uint64_t path_len, void *frame);
        return finte_proc_exec(p, a0, a1, frame);
    }
    if (nr == FPROC_SYS_KILL) {
        return (uint64_t)sys_kill(p, a0, a1);
    }
    if (nr == FPROC_SYS_SIGNAL) {
        return (uint64_t)sys_signal(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_SIGPROCMASK) {
        return sys_sigprocmask(p, a0, a1);
    }
    if (nr == FPROC_SYS_SIGRETURN) {
        const fproc_arch_ops_t *ops = fproc_arch();
        if (!p || !p->sig_frame || !ops->signal_restore) {
            return (uint64_t)-1;
        }
        p->sig_ret = ops->signal_restore(frame, p->sig_frame);
        return p->sig_ret;
    }
    if (nr == FPROC_SYS_GETPID) {
        return p ? p->pid : 1u;
    }
    if (nr == FPROC_SYS_GETITIMER) {
        return (uint64_t)sys_getitimer(p, a0, a1);
    }
    if (nr == FPROC_SYS_ALARM) {
        return sys_alarm(p, a0);
    }
    if (nr == FPROC_SYS_SETITIMER) {
        return (uint64_t)sys_setitimer(p, a0, a1, a2);
    }
    if (nr == FPROC_SYS_WAITPID) {
        return (uint64_t)finte_proc_waitpid(p, (int32_t)a0, a1, (int)a2);
    }
    if (nr == FPROC_SYS_SETPGID) {
        return (uint64_t)sys_setpgid(p, a0, a1);
    }
    if (nr == FPROC_SYS_GETPGRP) {
        return sys_getpgrp(p);
    }
    if (nr == FPROC_SYS_EXIT) {
        if (p && f) {
            finte_flow_exit(f, (int)a0);
        }
        (void)finte_disk_sync();
        hal_exit(0);
    }
    if (nr == FPROC_SYS_TIME) {
        return hal_now_ns();
    }
    if (nr == FPROC_SYS_SLEEP) {
        if (!p || !f) {
            return (uint64_t)-1;
        }
        uint64_t ns = a0;
        if (ns > 5000000000ull) {
            ns = 5000000000ull;
        }
        finte_flow_sleep(f, ns);
        return 0;
    }
    if (nr == FPROC_SYS_YIELD) {
        if (!p || !f) {
            return (uint64_t)-1;
        }
        finte_flow_yield(f);
        return 0;
    }
    if (nr == FPROC_SYS_IPC_SEND) {
        if (!p || !f || a2 > FINTE_MSG_DATA) {
            return (uint64_t)-1;
        }
        fmsg_t msg;
        finte_memset(&msg, 0, sizeof(msg));
        msg.len = (uint16_t)a2;
        if (finte_user_copy_in(f, msg.data, a1, (size_t)a2) != 0) {
            return (uint64_t)-1;
        }
        fret_t r = finte_chan_send(f, (uint16_t)a0, &msg, 0);
        return finte_failed(r) ? (uint64_t)-1 : a2;
    }
    if (nr == FPROC_SYS_IPC_SEND_CAP) {
        if (!p || !f) {
            return (uint64_t)-1;
        }
        fmsg_t msg;
        finte_memset(&msg, 0, sizeof(msg));
        msg.cap_count = 1;
        msg.caps[0] = (uint16_t)a1;
        msg.len = 0;
        fret_t r = finte_chan_send(f, (uint16_t)a0, &msg, 0);
        return finte_failed(r) ? (uint64_t)-1 : a1;
    }
    if (nr == FPROC_SYS_IPC_RECV) {
        if (!p || !f) {
            return (uint64_t)-1;
        }
        fmsg_t msg;
        fret_t r = finte_chan_recv(f, (uint16_t)a0, &msg, 0);
        if (finte_failed(r)) {
            return (uint64_t)-1;
        }
        uint64_t n = msg.len;
        if (n > a2) {
            n = a2;
        }
        if (n && finte_user_copy_out(f, a1, msg.data, (size_t)n) != 0) {
            return (uint64_t)-1;
        }
        if (msg.cap_count > 0 && a2 == 0) {
            return msg.caps[0];
        }
        return n;
    }
    if (nr == FPROC_SYS_IPC_RECV_CAP) {
        if (!p || !f) {
            return (uint64_t)-1;
        }
        fmsg_t msg;
        fret_t r = finte_chan_recv(f, (uint16_t)a0, &msg, 0);
        if (finte_failed(r) || msg.cap_count == 0) {
            return (uint64_t)-1;
        }
        return msg.caps[0];
    }
    if (nr == FPROC_SYS_SHM_ADDR) {
        return p ? FPROC_SHARED_VA : 0;
    }
    if (nr == FPROC_SYS_ARENA_MAP) {
        if (!p || !f) {
            return (uint64_t)-1;
        }
        fcap_t c;
        if (finte_cap_check(f, (uint16_t)a0, FINTE_R_WRITE, &c) < 0 ||
            !(c.flags & FINTE_CAP_SCOPED) || c.length == 0 ||
            c.length > FPROC_ARENA_MAX_PAGES * 4096ull) {
            return (uint64_t)-1;
        }
        fnode_t *n = finte_node_get(c.handle);
        if (!n || n->type != FINTE_NODE_ARENA) {
            return (uint64_t)-1;
        }
        farena_t *a = &finte_core.arenas[n->index];
        if (c.offset + c.length > a->size) {
            return (uint64_t)-1;
        }
        for (uint64_t off = 0; off < c.length; off += 4096u) {
            uint64_t pa = (uint64_t)(uintptr_t)(a->base + c.offset + off);
            if (finte_as_map(p->space, FPROC_ARENA_VA + off, pa,
                             FPROC_PROT_READ | FPROC_PROT_WRITE) != 0) {
                return (uint64_t)-1;
            }
        }
        return FPROC_ARENA_VA + FINTE_ARENA_BLOCK_HDR;
    }
    return (uint64_t)-1;
}

static void fork_user_entry(fflow_t *f)
{
    fproc_t *p = f ? (fproc_t *)f->user : NULL;
    if (!p || !f->thread || !f->thread->ctx) {
        finte_flow_exit(f, 1);
    }
    fhal_ctx_t *ctx = f->thread->ctx;
    ctx->addr_space = finte_as_root(p->space);
    finte_as_switch(p->space);
    uint64_t kstack_top = (uint64_t)(uintptr_t)ctx->stack + ctx->stack_size;
    fproc_arch()->enter_frame(p->fork_frame, kstack_top);
}

static const fprogram_t g_fork_program = {
    "fork-child", fork_user_entry, 1u << 20
};

static int proc_clone_fds(fproc_t *child, const fproc_t *parent)
{
    if (!child || !parent || !parent->fds) {
        return 0;
    }
    if (fd_table_grow(child, parent->fd_capacity) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < parent->fd_capacity; i++) {
        if (!parent->fds[i].ofd) {
            continue;
        }
        ffile_desc_t *o = parent->fds[i].ofd;
        ofd_ref(o);
        if (fd_bind(child, (int)i, o, parent->fds[i].fd_flags) != 0) {
            ofd_unref(o);
            return -1;
        }
    }
    return 0;
}

uint64_t finte_proc_fork(fproc_t *parent, void *frame)
{
    if (!parent || !parent->space || !parent->flow || !frame) {
        return (uint64_t)-1;
    }
    fproc_t *child = proc_alloc();
    if (!child) {
        return (uint64_t)-1;
    }
    child->pid = proc_alloc_pid();
    child->cpu = parent->cpu;
    child->entry = parent->entry;
    child->stack_top = parent->stack_top;
    child->shared_phys = parent->shared_phys;
    child->mmap_next = parent->mmap_next;
    child->fork_child = 1;
    finte_format(child->name, sizeof(child->name), "%s-child", parent->name);
    finte_memcpy(child->maps, parent->maps, sizeof(child->maps));
    finte_memcpy(child->sigs, parent->sigs, sizeof(child->sigs));
    child->space = finte_as_clone(parent->space);
    if (!child->space) {
        hal_pages_free(child, PROC_BYTES);
        return (uint64_t)-1;
    }
    if (parent->shared_phys) {
        (void)finte_as_map(child->space, FPROC_SHARED_VA, parent->shared_phys,
                           FPROC_PROT_READ | FPROC_PROT_WRITE);
    }
    child->sig_frame = hal_pages_alloc(4096);
    child->fork_frame = hal_pages_alloc(4096);
    if (!child->sig_frame || !child->fork_frame) {
        finte_as_unref(child->space);
        if (child->sig_frame) {
            hal_pages_free(child->sig_frame, 4096);
        }
        if (child->fork_frame) {
            hal_pages_free(child->fork_frame, 4096);
        }
        hal_pages_free(child, PROC_BYTES);
        return (uint64_t)-1;
    }
    finte_memset(child->sig_frame, 0, 4096);
    finte_memset(child->fork_frame, 0, 4096);
    fproc_arch()->fork_frame(child->fork_frame, frame);
    child->flow_handle = finte_flow_create(child->name, &g_fork_program,
                                           1u << 20, FINTE_PERSONA_NATIVE);
    if (!finte_handle_valid(child->flow_handle)) {
        finte_as_unref(child->space);
        hal_pages_free(child->sig_frame, 4096);
        hal_pages_free(child->fork_frame, 4096);
        hal_pages_free(child, PROC_BYTES);
        return (uint64_t)-1;
    }
    child->flow = finte_flow_lookup(child->flow_handle);
    if (!child->flow) {
        finte_node_unref(child->flow_handle);
        finte_as_unref(child->space);
        hal_pages_free(child->sig_frame, 4096);
        hal_pages_free(child->fork_frame, 4096);
        hal_pages_free(child, PROC_BYTES);
        return (uint64_t)-1;
    }
    child->flow->user = child;
    child->parent = parent;
    child->pgid = parent->pgid;
    child->sig_blocked = parent->sig_blocked;
    for (uint32_t i = 1; i < FPROC_MAX_SIGNALS; i++) {
        child->sigs[i].handler = parent->sigs[i].handler;
        child->sigs[i].restorer = parent->sigs[i].restorer;
    }
    if (child->flow->thread) {
        child->flow->thread->priority = 0;
        child->flow->thread->cpu = parent->cpu;
    }
    if (proc_clone_fds(child, parent) != 0) {
        finte_proc_destroy(child);
        return (uint64_t)-1;
    }
    child->next_sibling = parent->children;
    parent->children = child;
    proc_table_add(child);
    if (finte_proc_start(child) != 0) {
        parent->children = child->next_sibling;
        finte_proc_destroy(child);
        return (uint64_t)-1;
    }
    return child->pid;
}

static int proc_child_matches(fproc_t *parent, fproc_t *c, int32_t pid)
{
    if (pid > 0) {
        return c->pid == (uint32_t)pid;
    }
    if (pid == 0) {
        return c->pgid == parent->pgid;
    }
    if (pid == -1) {
        return 1;
    }
    return c->pgid == (uint32_t)(-(int64_t)pid);
}

enum { FPROC_WAIT_NONE, FPROC_WAIT_EXIT, FPROC_WAIT_STOP, FPROC_WAIT_CONT };

/* Pick the first matching child with a reportable event. Exits win over
 * stop/continue events only when encountered first; POSIX allows any
 * eligible child to be reported. */
static fproc_t *proc_wait_scan(fproc_t *p, int32_t pid, int options,
                               int *event, fproc_t **block_on)
{
    fproc_t *match = NULL;
    *event = FPROC_WAIT_NONE;
    *block_on = NULL;
    for (fproc_t *c = p->children; c; c = c->next_sibling) {
        if (!proc_child_matches(p, c, pid)) {
            continue;
        }
        if (!match) {
            match = c;
        }
        if (!c->flow) {
            *event = FPROC_WAIT_EXIT;
            return c;
        }
        if (!*block_on) {
            *block_on = c;
        }
        if ((options & FPROC_WUNTRACED) && c->stop_pending && c->stop_acked) {
            *event = FPROC_WAIT_STOP;
            return c;
        }
        if ((options & FPROC_WCONTINUED) && c->cont_pending) {
            *event = FPROC_WAIT_CONT;
            return c;
        }
    }
    return match;
}

int finte_proc_waitpid(fproc_t *p, int32_t pid, uint64_t status_uva,
                       int options)
{
    const int allowed = FPROC_WNOHANG | FPROC_WUNTRACED | FPROC_WCONTINUED;
    if (!p || !p->space || !p->flow || (options & ~allowed)) {
        return -1;
    }
    fproc_t *child = NULL;
    int event = FPROC_WAIT_NONE;
    for (;;) {
        fproc_t *block_on = NULL;
        child = proc_wait_scan(p, pid, options, &event, &block_on);
        if (!child) {
            return -1;
        }
        if (event != FPROC_WAIT_NONE) {
            break;
        }
        if (options & FPROC_WNOHANG) {
            return 0;
        }
        if (options & (FPROC_WUNTRACED | FPROC_WCONTINUED)) {
            /* Stop/continue acknowledgements have no wake channel; poll. */
            finte_flow_sleep(p->flow, 1000000ull);
        } else {
            /* Flow exit clears child->flow before waking waiters, and the
             * process keeps the flow handle referenced, so the flow object
             * cannot be recycled under us. */
            finte_flow_wait_for(p->flow, block_on->flow);
        }
    }
    int status = 0;
    if (event == FPROC_WAIT_EXIT) {
        status = child->faulted ? -1
                                : (int)((child->exit_code & 0xFFu) << 8);
    } else if (event == FPROC_WAIT_STOP) {
        status = (FPROC_SIGSTOP << 8) | 0x7F;
    } else {
        status = 0xFFFF;
    }
    if (status_uva != 0 &&
        proc_copy_out(p->space, status_uva, &status, sizeof(status)) != 0) {
        return -1;
    }
    if (event == FPROC_WAIT_STOP) {
        child->stop_pending = 0;
        return (int)child->pid;
    }
    if (event == FPROC_WAIT_CONT) {
        child->cont_pending = 0;
        return (int)child->pid;
    }
    fproc_t **link = &p->children;
    while (*link && *link != child) {
        link = &(*link)->next_sibling;
    }
    if (*link) {
        *link = child->next_sibling;
    }
    child->next_sibling = NULL;
    child->parent = NULL;
    uint32_t reaped = child->pid;
    finte_proc_destroy(child);
    return (int)reaped;
}

static void proc_exec_fds(fproc_t *p)
{
    if (!p || !p->fds) {
        return;
    }
    for (uint32_t i = 0; i < p->fd_capacity; i++) {
        if (p->fds[i].ofd &&
            (p->fds[i].fd_flags & FPROC_FD_CLOEXEC)) {
            fd_close(p, (int)i);
        }
    }
}

uint64_t finte_proc_exec(fproc_t *p, uint64_t path_uva, uint64_t path_len,
                         void *frame)
{
    if (!p || !p->space || !p->flow || !frame) {
        return (uint64_t)-1;
    }
    if (path_len == 0 || path_len >= FDISK_PATH_MAX) {
        return (uint64_t)-1;
    }
    char path[FDISK_PATH_MAX];
    if (proc_copy_in(p->space, path, path_uva, (size_t)path_len) != 0) {
        return (uint64_t)-1;
    }
    path[path_len] = 0;
    faddr_space_t *new = finte_as_create();
    if (!new) {
        return (uint64_t)-1;
    }
    uint64_t entry = 0;
    uint64_t stack_top = 0;
    if (proc_load_elf(new, path, &entry) != 0 ||
        proc_map_stack_into(new, &stack_top) != 0) {
        finte_as_unref(new);
        return (uint64_t)-1;
    }
    if (p->shared_phys) {
        if (finte_as_map(new, FPROC_SHARED_VA, p->shared_phys,
                         FPROC_PROT_READ | FPROC_PROT_WRITE) != 0) {
            finte_as_unref(new);
            return (uint64_t)-1;
        }
    }
    faddr_space_t *old = p->space;
    p->space = new;
    p->entry = entry;
    p->stack_top = stack_top;
    p->flow->space = new; /* transfer the creation reference */
    if (p->flow->thread && p->flow->thread->ctx) {
        p->flow->thread->ctx->addr_space = finte_as_root(new);
    }
    proc_init_maps(p);
    proc_exec_fds(p);
    finte_memset(p->sigs, 0, sizeof(p->sigs));
    finte_as_switch(new);
    finte_as_unref(old);
    fproc_arch()->exec_frame(frame, entry, stack_top - 16u);
    return 0;
}

void finte_proc_abort(fproc_t *p)
{
    if (!p) {
        return;
    }
    p->killed = 1;
    p->stopped = 0;
    p->stop_acked = 0;
    if (p->flow) {
        p->flow->abort = 1;
        if (p->flow->thread) {
            finte_sched_wake_thread(p->flow->thread);
        }
    }
}

int finte_proc_churn(uint32_t count)
{
    if (hal_cpu_self()->id != 0) {
        return FINTE_ERR_STATE;
    }
    proc_begin();
    uint64_t pm0 = pmm_active() ? pmm_free_page_count() : 0;
    int flow_ok = 1;
    for (uint32_t i = 0; i < count; i++) {
        fproc_t *p = finte_proc_create("churn", "exit.elf", 0, 0, 0, 0, 0, 0, 0);
        if (!p) {
            return FINTE_ERR_NOMEM;
        }
        if (p->flow && p->flow->thread) {
            p->flow->thread->cpu = 0;
            p->cpu = 0;
        }
        if (finte_proc_start(p) != 0) {
            finte_proc_destroy(p);
            return FINTE_ERR_STATE;
        }
        finte_run();
        finte_as_switch_kernel();
        if (finte_as_live_spaces() != 0 || p->exit_code != 0) {
            flow_ok = 0;
        }
        finte_proc_destroy(p);
    }
    uint64_t pm1 = pmm_active() ? pmm_free_page_count() : 0;
    uint32_t spaces = finte_as_live_spaces();
    char buf[160];
    int n = finte_format(buf, sizeof(buf),
                         "[procs] churn n=%u spaces=%u flow=%s pmm=%llu->%llu\n",
                         (unsigned)count, (unsigned)spaces,
                         flow_ok ? "OWNED" : "LEAK",
                         (unsigned long long)pm0, (unsigned long long)pm1);
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
    if (!flow_ok || spaces != 0 || (pmm_active() && pm1 != pm0)) {
        return FINTE_ERR_STATE;
    }
    return 0;
}

static const char *ok_str(int ok)
{
    return ok ? "OK" : "FAIL";
}

static void proc_begin(void)
{
    finte_as_switch_kernel();
    fproc_arch()->prepare();
}

int finte_usermode_run(const char *name)
{
    if (hal_cpu_self()->id != 0) {
        return FINTE_ERR_STATE;
    }
    proc_begin();
    uint64_t pm0 = pmm_active() ? pmm_free_page_count() : 0;
    fproc_t *p = finte_proc_create("user", name, 1, 0, 0, 0, 0, 0, 0);
    if (!p) {
        return FINTE_ERR_NOMEM;
    }
    if (p->flow && p->flow->thread) {
        p->flow->thread->cpu = 0;
        p->cpu = 0;
    }
    int rc = finte_proc_start(p);
    if (rc != 0) {
        finte_proc_destroy(p);
        return rc;
    }
    finte_run();
    finte_as_switch_kernel();
    if (proc_live_children(p)) {
        proc_abort_children(p);
        finte_run();
        finte_as_switch_kernel();
    }
    int status = p->faulted ? FINTE_ERR_FAULT : finte_run_status();
    if (status == FINTE_OK && p->exit_code != 0) {
        status = FINTE_ERR_STATE;
    }
    for (fproc_t *c = p->children; c; c = c->next_sibling) {
        if (c->faulted || c->exit_code != 0) {
            status = FINTE_ERR_STATE;
        }
    }
    finte_proc_destroy(p);
    uint64_t pm1 = pmm_active() ? pmm_free_page_count() : 0;
    if (pmm_active() && pm1 < pm0) {
        char buf[96];
        int n = finte_format(buf, sizeof(buf),
                             "[user] pmm leak %llu -> %llu\n",
                             (unsigned long long)pm0,
                             (unsigned long long)pm1);
        hal_console_write(buf, (size_t)n);
        status = FINTE_ERR_STATE;
    }
    return status;
}

int finte_proc_demo(void)
{
    if (hal_cpu_self()->id != 0) {
        return FINTE_ERR_STATE;
    }
    proc_begin();
    uint64_t pm0 = pmm_active() ? pmm_free_page_count() : 0;
    fhandle_t chan = finte_channel_create("proc-ipc", 4);
    if (!finte_handle_valid(chan)) {
        return FINTE_ERR_NOMEM;
    }
    demo_shm_t *shm = (demo_shm_t *)hal_pages_alloc(4096);
    if (!shm) {
        finte_node_unref(chan);
        return FINTE_ERR_NOMEM;
    }
    finte_memset(shm, 0, 4096);
    shm->magic = DEMO_SHM_MAGIC;
    fhandle_t arena = finte_arena_create("svc-arena", 2 * 4096);
    if (!finte_handle_valid(arena)) {
        hal_pages_free(shm, 4096);
        finte_node_unref(chan);
        return FINTE_ERR_NOMEM;
    }
    fproc_t *p1 = finte_proc_create("proc-ipc", "ipc.elf", 1, (uint64_t)(uintptr_t)shm,
                                    chan, FINTE_R_SEND, arena, 0, FINTE_R_WRITE);
    fproc_t *p2 = finte_proc_create("proc-rx", "ipc.elf", 2, (uint64_t)(uintptr_t)shm,
                                    chan, FINTE_R_RECV, arena, 1, FINTE_R_WRITE);
    fproc_t *p3 = finte_proc_create("proc-spin", "ipc.elf", 3, (uint64_t)(uintptr_t)shm,
                                    0, 0, 0, 0, 0);
    fproc_t *p4 = finte_proc_create("proc-watch", "ipc.elf", 4, (uint64_t)(uintptr_t)shm,
                                    0, 0, 0, 0, 0);
    int result = FINTE_ERR_NOMEM;
    if (!p1 || !p2 || !p3 || !p4) {
        goto out;
    }
    if (hal_cpu_count() > 1 && p4->flow) {
        uint32_t cpu = p4->flow->thread ? p4->flow->thread->cpu : 0;
        uint32_t target = (cpu + 1u) % hal_cpu_count();
        (void)finte_sched_migrate(p4->flow, target);
    }
    p1->expected_fault = 1;
    int rc = 0;
    rc |= finte_proc_start(p1);
    rc |= finte_proc_start(p2);
    rc |= finte_proc_start(p3);
    rc |= finte_proc_start(p4);
    if (rc != 0) {
        result = FINTE_ERR_STATE;
        goto out;
    }
    finte_run();
    finte_as_switch_kernel();
    int ipc = (shm->flags & DEMO_IPC_FLAG) != 0;
    int iso = (shm->flags & DEMO_ISO_FLAG) != 0;
    int shm_ok = (shm->flags & DEMO_SHM_FLAG) != 0 &&
                 shm->magic == DEMO_SHM_MAGIC;
    int time_ok = (shm->flags & DEMO_TIME_FLAG) != 0;
    int preempt = (shm->flags & DEMO_PREEMPT_FLAG) != 0 && shm->heartbeat > 0;
    int arena_ok = (shm->flags & (DEMO_ARENA_FLAG | DEMO_ARENA_DENY_FLAG)) ==
                   (DEMO_ARENA_FLAG | DEMO_ARENA_DENY_FLAG);
    int fault_ok = p1->faulted;
    char buf[256];
    int n = finte_format(buf, sizeof(buf),
                         "[procs] ipc=%s isolation=%s shm=%s arena=%s "
                         "time=%s preempt=%s fault=%s heartbeat=%llu\n",
                         ok_str(ipc), ok_str(iso), ok_str(shm_ok),
                         ok_str(arena_ok), ok_str(time_ok), ok_str(preempt),
                         ok_str(fault_ok),
                         (unsigned long long)shm->heartbeat);
    hal_console_write(buf, (size_t)n);
    result = (ipc && iso && shm_ok && arena_ok && time_ok && preempt &&
              fault_ok) ? 0 : FINTE_ERR_STATE;
out:
    finte_proc_destroy(p1);
    finte_proc_destroy(p2);
    finte_proc_destroy(p3);
    finte_proc_destroy(p4);
    finte_node_unref(arena);
    hal_pages_free(shm, 4096);
    finte_node_unref(chan);
    uint64_t pm1 = pmm_active() ? pmm_free_page_count() : 0;
    if (pmm_active() && pm1 != pm0) {
        char buf[128];
        int n = finte_format(buf, sizeof(buf),
                             "[procs] pmm free %llu -> %llu\n",
                             (unsigned long long)pm0,
                             (unsigned long long)pm1);
        hal_console_write(buf, (size_t)n);
        result = FINTE_ERR_STATE;
    } else if (pmm_active()) {
        char buf[96];
        int n = finte_format(buf, sizeof(buf),
                             "[procs] pmm free stable=%llu\n",
                             (unsigned long long)pm1);
        hal_console_write(buf, (size_t)n);
    }
    return result;
}

static int proc_cap_test(void)
{
    proc_begin();
    fhandle_t chan = finte_channel_create("cap-ipc", 4);
    fhandle_t reply = finte_channel_create("reply-ipc", 4);
    if (!finte_handle_valid(chan) || !finte_handle_valid(reply)) {
        if (finte_handle_valid(chan)) {
            finte_node_unref(chan);
        }
        if (finte_handle_valid(reply)) {
            finte_node_unref(reply);
        }
        return FINTE_ERR_NOMEM;
    }
    fproc_t *p1 = finte_proc_create("cap-tx", "cap.elf", 1, 0, chan,
                                    FINTE_R_SEND, 0, 0, 0);
    fproc_t *p2 = finte_proc_create("cap-rx", "cap.elf", 2, 0, chan,
                                    FINTE_R_RECV, 0, 0, 0);
    if (p1 && p1->flow) {
        (void)finte_cap_install(p1->flow, reply,
                                FINTE_R_SEND | FINTE_R_RECV);
    }
    if (p1 && p1->flow && p1->flow->thread) {
        p1->flow->thread->cpu = 0;
        p1->cpu = 0;
    }
    if (p2 && p2->flow && p2->flow->thread) {
        p2->flow->thread->cpu = 0;
        p2->cpu = 0;
    }
    int ok = 0;
    if (p1 && p2 && finte_proc_start(p1) == 0 && finte_proc_start(p2) == 0) {
        finte_run();
        finte_as_switch_kernel();
        ok = !p1->faulted && !p2->faulted &&
             p1->exit_code == 0 && p2->exit_code == 0;
    }
    finte_proc_destroy(p1);
    finte_proc_destroy(p2);
    finte_node_unref(chan);
    finte_node_unref(reply);
    return ok ? 0 : FINTE_ERR_STATE;
}

static int proc_service_test(void)
{
    proc_begin();
    fhandle_t req = finte_channel_create("svc-echo", 4);
    fhandle_t rep = finte_channel_create("svc-echo-reply", 4);
    fhandle_t own = finte_channel_create("svc-self", 4);
    if (!finte_handle_valid(req) || !finte_handle_valid(rep) ||
        !finte_handle_valid(own)) {
        if (finte_handle_valid(req)) {
            finte_node_unref(req);
        }
        if (finte_handle_valid(rep)) {
            finte_node_unref(rep);
        }
        if (finte_handle_valid(own)) {
            finte_node_unref(own);
        }
        return FINTE_ERR_NOMEM;
    }
    fproc_t *provider = finte_proc_create("svc-provider", "svc.elf", 1, 0,
                                          req, FINTE_R_SEND | FINTE_R_RECV,
                                          0, 0, 0);
    fproc_t *client = finte_proc_create("svc-client", "svc_client.elf", 2, 0,
                                        own, FINTE_R_SEND | FINTE_R_RECV,
                                        0, 0, 0);
    int ok = 0;
    if (provider && client && provider->flow && client->flow) {
        /* Two channels keep request and reply directions apart, so a fast
         * client cannot consume its own request on an SMP boot. Register the
         * provider before either flow starts so lookup is deterministic. */
        ok = finte_cap_install(provider->flow, rep,
                               FINTE_R_SEND | FINTE_R_RECV) == 1 &&
             finte_service_register(provider, "echo", 0) == 0 &&
             finte_service_register(provider, "echo-reply", 1) == 0;
        if (ok && finte_proc_start(provider) == 0 &&
            finte_proc_start(client) == 0) {
            finte_run();
            finte_as_switch_kernel();
            ok = !provider->faulted && !client->faulted &&
                 provider->exit_code == 0 && client->exit_code == 0 &&
                 finte_service_count() == 2;
        } else {
            ok = 0;
        }
    }
    finte_proc_destroy(provider);
    finte_proc_destroy(client);
    if (finte_service_count() != 0) {
        ok = 0;
    }
    finte_node_unref(req);
    finte_node_unref(rep);
    finte_node_unref(own);
    return ok ? 0 : FINTE_ERR_STATE;
}

static int proc_signal_test(void)
{
    proc_begin();
    fproc_t *rx = finte_proc_create("signal", "signal.elf", 1, 0, 0, 0, 0, 0, 0);
    fproc_t *tx = finte_proc_create("killer", "kill.elf", 2, 0, 0, 0, 0, 0, 0);
    if (rx && rx->flow && rx->flow->thread) {
        rx->flow->thread->cpu = 0;
        rx->cpu = 0;
    }
    if (tx && tx->flow && tx->flow->thread) {
        tx->flow->thread->cpu = 0;
        tx->cpu = 0;
    }
    int ok = 0;
    if (rx && tx && finte_proc_start(rx) == 0 && finte_proc_start(tx) == 0) {
        finte_run();
        finte_as_switch_kernel();
        ok = !rx->faulted && !tx->faulted &&
             rx->exit_code == 0 && tx->exit_code == 0;
    }
    finte_proc_destroy(rx);
    finte_proc_destroy(tx);
    return ok ? 0 : FINTE_ERR_STATE;
}

/* Cross-architecture process ABI test: PTE protection, fd/mmap, fork, exec,
 * signal delivery, churn and abort of a blocked process. */
int finte_proc_abi_test(void)
{
    /* The suite deliberately provokes user faults and aborts blocked flows.
     * Those are expected events, so keep the three-strike watchdog from
     * turning them into a machine panic. */
    hal_watchdog_suspend();
    int space_ok = 0;
    faddr_space_t *as = finte_as_create();
    if (as) {
        void *rx = finte_as_alloc_page(as);
        void *rw = finte_as_alloc_page(as);
        if (rx && rw &&
            finte_as_map(as, FPROC_USER_BASE, (uint64_t)(uintptr_t)rx,
                         FPROC_PROT_READ | FPROC_PROT_EXEC) == 0 &&
            finte_as_map(as, FPROC_USER_BASE + 4096u,
                         (uint64_t)(uintptr_t)rw,
                         FPROC_PROT_READ | FPROC_PROT_WRITE) == 0) {
            uint32_t rxp = finte_as_prot(as, FPROC_USER_BASE);
            uint32_t rwp = finte_as_prot(as, FPROC_USER_BASE + 4096u);
            space_ok = (rxp & FPROC_PROT_EXEC) &&
                       !(rxp & FPROC_PROT_WRITE) &&
                       finte_as_translate(as, FPROC_USER_BASE, 1) == 0 &&
                       finte_as_translate(as, FPROC_USER_BASE, 0) != 0 &&
                       (rwp & FPROC_PROT_WRITE) &&
                       !(rwp & FPROC_PROT_EXEC) &&
                       finte_as_translate(as, FPROC_USER_BASE + 4096u, 1) != 0;
        }
        finte_as_unref(as);
    }
    int abi = finte_usermode_run("abi.elf");
    hal_console_write("[abi] stage fd/mmap done\n", 26);
    int fork_rc = finte_usermode_run("fork.elf");
    hal_console_write("[abi] stage fork done\n", 23);
    int wait_rc = finte_usermode_run("wait.elf");
    hal_console_write("[abi] stage wait done\n", 23);
    int cow_rc = finte_usermode_run("cow.elf");
    hal_console_write("[abi] stage cow done\n", 22);
    int filemap_rc = finte_usermode_run("filemap.elf");
    hal_console_write("[abi] stage filemap done\n", 26);
    int exec_rc = finte_usermode_run("exec.elf");
    hal_console_write("[abi] stage exec done\n", 23);
    int sig_rc = proc_signal_test();
    hal_console_write("[abi] stage signal done\n", 25);
    int sigset_rc = finte_usermode_run("sigset.elf");
    hal_console_write("[abi] stage sigset done\n", 25);
    int job_rc = finte_usermode_run("job.elf");
    hal_console_write("[abi] stage job done\n", 22);
    int pgroup_rc = finte_usermode_run("pgroup.elf");
    hal_console_write("[abi] stage pgroup done\n", 25);
    int alarm_rc = finte_usermode_run("alarm.elf");
    hal_console_write("[abi] stage alarm done\n", 24);
    int fdtab_rc = finte_usermode_run("fdtab.elf");
    hal_console_write("[abi] stage fdtab done\n", 24);
    int cap_rc = proc_cap_test();
    hal_console_write("[abi] stage cap done\n", 22);
    int svc_rc = proc_service_test();
    hal_console_write("[abi] stage service done\n", 26);
    int bad_rc = finte_usermode_run("bad.elf");
    int dyn_rc = finte_usermode_run("dyn.elf");
    int churn_rc = finte_proc_churn(8);

    /* A process blocked in ipc_recv must keep its space alive until aborted,
     * then release it through the normal flow-death path. */
    int blocked_ok = 0;
    fhandle_t chan = finte_channel_create("abi-block", 1);
    fproc_t *blocked = finte_proc_create("blocked", "block.elf", 0, 0, chan,
                                         FINTE_R_RECV, 0, 0, 0);
    if (blocked && blocked->flow && blocked->flow->thread) {
        blocked->flow->thread->cpu = 0;
        blocked->cpu = 0;
    }
    if (blocked && finte_proc_start(blocked) == 0) {
        finte_run();
        uint32_t held = finte_as_live_spaces();
        finte_proc_abort(blocked);
        finte_run();
        finte_as_switch_kernel();
        blocked_ok = held >= 1 && finte_as_live_spaces() == 0;
        finte_proc_destroy(blocked);
    }
    if (finte_handle_valid(chan)) {
        finte_node_unref(chan);
    }

    char buf[256];
    int n = finte_format(buf, sizeof(buf),
                         "[abi] space=%s user=%s fork=%s wait=%s cow=%s "
                         "filemap=%s exec=%s signal=%s sigset=%s job=%s "
                         "pgroup=%s alarm=%s fdtab=%s cap=%s service=%s "
                         "bad=%s dyn=%s churn=%s blocked=%s\n",
                         ok_str(space_ok), ok_str(abi == 0),
                         ok_str(fork_rc == 0), ok_str(wait_rc == 0),
                         ok_str(cow_rc == 0), ok_str(filemap_rc == 0),
                         ok_str(exec_rc == 0),
                         ok_str(sig_rc == 0), ok_str(sigset_rc == 0),
                         ok_str(job_rc == 0), ok_str(pgroup_rc == 0),
                         ok_str(alarm_rc == 0), ok_str(fdtab_rc == 0),
                         ok_str(cap_rc == 0), ok_str(svc_rc == 0),
                         ok_str(bad_rc == 0), ok_str(dyn_rc == 0),
                         ok_str(churn_rc == 0), ok_str(blocked_ok));
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
    hal_watchdog_resume();
    return (space_ok && abi == 0 && fork_rc == 0 && wait_rc == 0 &&
            cow_rc == 0 && filemap_rc == 0 && exec_rc == 0 &&
            sig_rc == 0 && sigset_rc == 0 && job_rc == 0 &&
            pgroup_rc == 0 && alarm_rc == 0 && fdtab_rc == 0 &&
            cap_rc == 0 && svc_rc == 0 &&
            bad_rc == 0 && dyn_rc == 0 && churn_rc == 0 &&
            blocked_ok) ? 0 : FINTE_ERR_STATE;
}
