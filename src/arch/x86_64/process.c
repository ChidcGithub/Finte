/* Isolated user processes, process-local syscalls and the IPC/time demo.
 *
 * Every process gets a private PML4.  The low 4 GiB identity map is copied in
 * from shared supervisor page tables so kernel code and physical memory stay
 * accessible to CPL0 only.  User pages are 4 KiB mappings at 4 GiB+, backed by
 * pages from the kernel heap. */
#include "finte/finte.h"
#include "finte/initrd.h"
#include "process.h"

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

#define PROC_STACK_PAGES 4
#define PROC_STACK_BYTES (PROC_STACK_PAGES * 4096u)
#define FPROC_MAX_PAGES 128

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

typedef struct fproc {
    uint32_t pid;
    uint32_t cpu;
    uint64_t pml4_phys;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t shared_phys;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *user_pd;
    fflow_t *flow;
    fhandle_t flow_handle;
    int ipc_cap;
    int arena_cap;
    int faulted;
    uint8_t expected_fault;
    char name[FINTE_NAME_MAX];
    uint64_t pages[FPROC_MAX_PAGES];
    uint32_t page_count;
} fproc_t;

static uint64_t g_kernel_pd[4][512] __attribute__((aligned(4096)));
static int g_kernel_pd_ready;
static uint64_t g_kernel_cr3;
static int g_user_faults;

extern void um_enter(uint64_t rip, uint64_t rsp, uint64_t flags)
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

static void *alloc_page(void)
{
    void *p = hal_pages_alloc(4096);
    if (p) {
        finte_memset(p, 0, 4096);
    }
    return p;
}

static void *proc_alloc_raw(fproc_t *p)
{
    void *page = alloc_page();
    if (!page) {
        return NULL;
    }
    if (p->page_count >= FPROC_MAX_PAGES) {
        hal_pages_free(page, 4096);
        return NULL;
    }
    p->pages[p->page_count++] = ptr_phys(page);
    return page;
}

static void proc_destroy(fproc_t *p)
{
    if (!p) {
        return;
    }
    if (finte_handle_valid(p->flow_handle) && p->flow) {
        finte_cap_drop_all(p->flow);
    }
    if (finte_handle_valid(p->flow_handle)) {
        finte_node_unref(p->flow_handle);
    }
    for (uint32_t i = 0; i < p->page_count; i++) {
        hal_pages_free((void *)(uintptr_t)p->pages[i], 4096);
    }
    p->page_count = 0;
    hal_pages_free(p, 4096);
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
            g_kernel_pd[i][j] = addr | 0x83u; /* present, rw, PS, supervisor */
        }
    }
    g_kernel_pd_ready = 1;
}

static uint64_t *proc_pte(fproc_t *p, uint64_t va, int create)
{
    if (!p || va < FPROC_USER_BASE || va >= FPROC_USER_LIMIT) {
        return NULL;
    }
    uint32_t pdi = (uint32_t)((va >> 21) & 0x1FFu);
    uint64_t pde = p->user_pd[pdi];
    if ((pde & 1u) == 0) {
        if (!create) {
            return NULL;
        }
        uint64_t *pt = (uint64_t *)proc_alloc_raw(p);
        if (!pt) {
            return NULL;
        }
        p->user_pd[pdi] = ptr_phys(pt) | 0x7u;
        pde = p->user_pd[pdi];
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & ~0xFFFull);
    return &pt[(va >> 12) & 0x1FFu];
}

static int proc_map_page(fproc_t *p, uint64_t va, uint64_t phys)
{
    uint64_t *pte = proc_pte(p, va & ~0xFFFull, 1);
    if (!pte) {
        return FINTE_ERR_NOMEM;
    }
    *pte = (phys & ~0xFFFull) | 0x7u; /* present, rw, user */
    return 0;
}

static void *proc_alloc_page(fproc_t *p, uint64_t va)
{
    void *page = proc_alloc_raw(p);
    if (!page) {
        return NULL;
    }
    if (proc_map_page(p, va, ptr_phys(page)) != 0) {
        return NULL;
    }
    return page;
}

static void *proc_page_ptr(fproc_t *p, uint64_t va, int alloc)
{
    uint64_t *pte = proc_pte(p, va & ~0xFFFull, alloc);
    if (!pte) {
        return NULL;
    }
    if ((*pte & 1u) == 0) {
        if (!alloc) {
            return NULL;
        }
        void *page = proc_alloc_raw(p);
        if (!page) {
            return NULL;
        }
        *pte = ptr_phys(page) | 0x7u;
    }
    return (void *)(uintptr_t)(*pte & ~0xFFFull);
}

static int proc_build_space(fproc_t *p)
{
    kernel_pd_init();
    p->pml4 = (uint64_t *)proc_alloc_raw(p);
    p->pdpt = (uint64_t *)proc_alloc_raw(p);
    p->user_pd = (uint64_t *)proc_alloc_raw(p);
    if (!p->pml4 || !p->pdpt || !p->user_pd) {
        return FINTE_ERR_NOMEM;
    }
    p->pml4[0] = ptr_phys(p->pdpt) | 0x7u;
    for (uint32_t i = 0; i < 4; i++) {
        p->pdpt[i] = ptr_phys(g_kernel_pd[i]) | 0x3u; /* supervisor */
    }
    p->pdpt[4] = ptr_phys(p->user_pd) | 0x7u;
    p->pml4_phys = ptr_phys(p->pml4);
    return 0;
}

static int proc_map_stack(fproc_t *p)
{
    for (uint32_t i = 0; i < PROC_STACK_PAGES; i++) {
        uint64_t va = FPROC_STACK_TOP - (uint64_t)(i + 1) * 4096u;
        if (!proc_alloc_page(p, va)) {
            return FINTE_ERR_NOMEM;
        }
    }
    p->stack_top = FPROC_STACK_TOP;
    return 0;
}

static int proc_load_elf(fproc_t *p, const char *name)
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
    if (eh->machine != 62 || eh->type != 2 ||
        eh->phentsize < sizeof(elf64_phdr_t)) {
        return FINTE_ERR_TYPE;
    }
    uint64_t code_limit = FPROC_STACK_TOP - PROC_STACK_BYTES;
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
        for (uint64_t j = 0; j < ph->memsz; j++) {
            uint64_t va = ph->vaddr + j;
            uint8_t *page = (uint8_t *)proc_page_ptr(p, va, 1);
            if (!page) {
                char dbg[96];
                int dn = finte_format(dbg, sizeof(dbg),
                                      "[procs] map fail va=0x%llx j=0x%llx\n",
                                      (unsigned long long)va,
                                      (unsigned long long)j);
                hal_console_write(dbg, (size_t)dn);
                return FINTE_ERR_NOMEM;
            }
            page[va & 0xFFFu] =
                j < ph->filesz ? image[ph->offset + j] : 0;
        }
    }
    if (eh->entry < FPROC_USER_BASE || eh->entry >= code_limit) {
        return FINTE_ERR_RANGE;
    }
    p->entry = eh->entry;
    return 0;
}

static int proc_user_ok(fproc_t *p, uint64_t va, uint64_t len, int write)
{
    if (len == 0) {
        return 1;
    }
    if (va + len < va) {
        return 0;
    }
    if (!p) {
        /* Legacy single-address-space ring-3 program: the identity map has
         * user bits below 64 MiB only. */
        return va + len <= 0x4000000ull;
    }
    if (va < FPROC_USER_BASE || va + len > FPROC_USER_LIMIT) {
        return 0;
    }
    for (uint64_t page = va & ~0xFFFull; page < va + len; page += 4096u) {
        uint64_t *pte = proc_pte(p, page, 0);
        uint64_t need = 1u | (write ? 2u : 0u) | 4u;
        if (!pte || (*pte & need) != need) {
            return 0;
        }
    }
    return 1;
}

static int proc_copy_in(fproc_t *p, void *dst, uint64_t uva, size_t len)
{
    if (!proc_user_ok(p, uva, len, 0)) {
        return -1;
    }
    if (!p) {
        finte_memcpy(dst, (const void *)(uintptr_t)uva, len);
        return 0;
    }
    while (len) {
        uint64_t page = uva & ~0xFFFull;
        uint64_t *pte = proc_pte(p, page, 0);
        size_t off = (size_t)(uva - page);
        size_t chunk = 4096u - off;
        if (chunk > len) {
            chunk = len;
        }
        finte_memcpy(dst, (const void *)(uintptr_t)((*pte & ~0xFFFull) + off),
                     chunk);
        dst = (uint8_t *)dst + chunk;
        uva += chunk;
        len -= chunk;
    }
    return 0;
}

static int proc_copy_out(fproc_t *p, uint64_t uva, const void *src, size_t len)
{
    if (!proc_user_ok(p, uva, len, 1)) {
        return -1;
    }
    if (!p) {
        finte_memcpy((void *)(uintptr_t)uva, src, len);
        return 0;
    }
    while (len) {
        uint64_t page = uva & ~0xFFFull;
        uint64_t *pte = proc_pte(p, page, 0);
        size_t off = (size_t)(uva - page);
        size_t chunk = 4096u - off;
        if (chunk > len) {
            chunk = len;
        }
        finte_memcpy((void *)(uintptr_t)((*pte & ~0xFFFull) + off), src, chunk);
        src = (const uint8_t *)src + chunk;
        uva += chunk;
        len -= chunk;
    }
    return 0;
}

int finte_user_copy_in(fflow_t *f, void *dst, uint64_t uva, size_t len)
{
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
    return proc_copy_in(p, dst, uva, len);
}

int finte_user_copy_out(fflow_t *f, uint64_t uva, const void *src, size_t len)
{
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
    return proc_copy_out(p, uva, src, len);
}

static void user_process_entry(fflow_t *f)
{
    fproc_t *p = f ? (fproc_t *)f->user : NULL;
    if (!p) {
        finte_flow_exit(f, 1);
    }
    f->ctx->cr3 = p->pml4_phys;
    write_cr3(p->pml4_phys);
    um_enter(p->entry, p->stack_top - 16u, 0x202u);
}

static const fprogram_t g_user_program = {
    "user-process", user_process_entry, 1u << 20
};

static fproc_t *proc_create(const char *name, const char *elf, uint32_t pid,
                            uint32_t cpu, uint64_t shared_phys,
                            fhandle_t chan, uint32_t rights,
                            fhandle_t arena, uint32_t arena_page,
                            uint32_t arena_rights)
{
    static int stage;
    static int last_rc;
    stage = 1;
    last_rc = 0;
    fproc_t *p = (fproc_t *)alloc_page();
    if (!p) {
        return NULL;
    }
    p->pid = pid;
    p->cpu = cpu;
    p->shared_phys = shared_phys;
    p->ipc_cap = -1;
    p->arena_cap = -1;
    finte_strlcpy(p->name, name, sizeof(p->name));
    stage = 2;
    last_rc = proc_build_space(p);
    if (last_rc != 0) {
        goto fail;
    }
    stage = 3;
    last_rc = proc_load_elf(p, elf);
    if (last_rc != 0) {
        goto fail;
    }
    stage = 4;
    last_rc = proc_map_stack(p);
    if (last_rc != 0) {
        goto fail;
    }
    stage = 5;
    if (shared_phys && proc_map_page(p, FPROC_SHARED_VA, shared_phys) != 0) {
        last_rc = FINTE_ERR_NOMEM;
        goto fail;
    }
    stage = 6;
    p->flow_handle = finte_flow_create(name, &g_user_program,
                                       1u << 20, FINTE_PERSONA_NATIVE);
    if (!finte_handle_valid(p->flow_handle)) {
        last_rc = FINTE_ERR_NOMEM;
        goto fail;
    }
    stage = 7;
    p->flow = finte_flow_lookup(p->flow_handle);
    if (!p->flow) {
        last_rc = FINTE_ERR_NOENT;
        goto fail;
    }
    p->flow->user = p;
    /* The TSS is global: user processes run only on the BSP until per-CPU
     * TSS/GDT support lands.  Kernel flows still use every CPU. */
    (void)cpu;
    p->cpu = 0;
    p->flow->cpu = 0;
    p->flow->priority = 0;
    stage = 8;
    if (finte_handle_valid(chan) && rights) {
        int slot = finte_cap_install(p->flow, chan, rights);
        if (slot < 0) {
            last_rc = slot;
            goto fail;
        }
        p->ipc_cap = slot;
    }
    if (finte_handle_valid(arena) && arena_rights) {
        int slot = finte_cap_install_arena_pages(p->flow, arena, arena_page, 1,
                                                 arena_rights);
        if (slot < 0) {
            last_rc = slot;
            goto fail;
        }
        p->arena_cap = slot;
    }
    return p;

fail: {
        char buf[96];
        int n = finte_format(buf, sizeof(buf),
                             "[procs] pid %u create stage %d rc %d\n",
                             (unsigned)pid, stage, last_rc);
        hal_console_write(buf, (size_t)n);
    }
    if (finte_handle_valid(p->flow_handle) && p->flow) {
        finte_cap_drop_all(p->flow);
    }
    if (finte_handle_valid(p->flow_handle)) {
        finte_node_unref(p->flow_handle);
    }
    proc_destroy(p);
    return NULL;
}

static int proc_start(fproc_t *p)
{
    return finte_flow_start(p->flow_handle).status;
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
                         "rip=0x%llx cr2=0x%llx\n",
                         (unsigned)p->pid,
                         (unsigned long long)vec,
                         (unsigned long long)rip,
                         (unsigned long long)cr2);
    hal_console_write(buf, (size_t)n);
    p->faulted = 1;
    g_user_faults++;
    if (!p->expected_fault) {
        char reason[64];
        int rn = finte_format(reason, sizeof(reason),
                              "user fault pid %u vec %llu",
                              (unsigned)p->pid,
                              (unsigned long long)vec);
        if (rn > 0) {
            hal_watchdog_strike(reason);
        }
    }
    finte_flow_exit(f, 128 + (int)vec);
}

uint64_t finte_process_syscall(uint64_t *regs)
{
    uint64_t nr = regs[14];
    uint64_t a0 = regs[9];  /* rdi */
    uint64_t a1 = regs[10]; /* rsi */
    uint64_t a2 = regs[11]; /* rdx */
    fflow_t *f = finte_flow_current();
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;

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
    if (nr == FPROC_SYS_GETPID) {
        return p ? p->pid : 1u;
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
        if (finte_user_copy_out(f, a1, msg.data, (size_t)n) != 0) {
            return (uint64_t)-1;
        }
        return n;
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
            if (proc_map_page(p, FPROC_ARENA_VA + off,
                              ptr_phys(a->base + c.offset + off)) != 0) {
                return (uint64_t)-1;
            }
        }
        return FPROC_ARENA_VA + FINTE_ARENA_BLOCK_HDR;
    }
    return (uint64_t)-1;
}

static const char *ok_str(int ok)
{
    return ok ? "OK" : "FAIL";
}

static void proc_begin(void)
{
    if (g_kernel_cr3 == 0) {
        g_kernel_cr3 = read_cr3();
    }
    g_user_faults = 0;
    usermode_prepare();
}

int finte_usermode_run(const char *name)
{
    if (hal_cpu_self()->id != 0) {
        return FINTE_ERR_STATE;
    }
    proc_begin();
    fproc_t *p = proc_create("user", name, 1, 0, 0, 0, 0, 0, 0, 0);
    if (!p) {
        return FINTE_ERR_NOMEM;
    }
    int rc = proc_start(p);
    if (rc != 0) {
        proc_destroy(p);
        return rc;
    }
    finte_run();
    write_cr3(g_kernel_cr3);
    int status = g_user_faults ? FINTE_ERR_FAULT : finte_run_status();
    proc_destroy(p);
    return status;
}

int finte_proc_demo(void)
{
    if (hal_cpu_self()->id != 0) {
        return FINTE_ERR_STATE;
    }
    proc_begin();

    fhandle_t chan = finte_channel_create("proc-ipc", 4);
    if (!finte_handle_valid(chan)) {
        return FINTE_ERR_NOMEM;
    }
    demo_shm_t *shm = (demo_shm_t *)alloc_page();
    if (!shm) {
        finte_node_unref(chan);
        return FINTE_ERR_NOMEM;
    }
    shm->magic = DEMO_SHM_MAGIC;

    fhandle_t arena = finte_arena_create("svc-arena", 2 * 4096);
    if (!finte_handle_valid(arena)) {
        hal_pages_free(shm, 4096);
        finte_node_unref(chan);
        return FINTE_ERR_NOMEM;
    }

    fproc_t *p1 = NULL;
    fproc_t *p2 = NULL;
    fproc_t *p3 = NULL;
    fproc_t *p4 = NULL;
    int result = FINTE_ERR_NOMEM;

    p1 = proc_create("proc-ipc", "ipc.elf", 1, 0,
                     ptr_phys(shm), chan, FINTE_R_SEND,
                     arena, 0, FINTE_R_WRITE);
    p2 = proc_create("proc-rx", "ipc.elf", 2, 0,
                     ptr_phys(shm), chan, FINTE_R_RECV,
                     arena, 1, FINTE_R_WRITE);
    p3 = proc_create("proc-spin", "ipc.elf", 3, 0,
                     ptr_phys(shm), 0, 0, 0, 0, 0);
    p4 = proc_create("proc-watch", "ipc.elf", 4, 0,
                     ptr_phys(shm), 0, 0, 0, 0, 0);
    if (!p1 || !p2 || !p3 || !p4) {
        goto out;
    }
    /* PID 1 probes supervisor memory on purpose: contained and expected. */
    p1->expected_fault = 1;

    int rc = 0;
    rc |= proc_start(p1);
    rc |= proc_start(p2);
    rc |= proc_start(p3);
    rc |= proc_start(p4);
    if (rc != 0) {
        result = FINTE_ERR_STATE;
        goto out;
    }

    finte_run();
    write_cr3(g_kernel_cr3);

    int ipc = (shm->flags & DEMO_IPC_FLAG) != 0;
    int iso = (shm->flags & DEMO_ISO_FLAG) != 0;
    int shm_ok = (shm->flags & DEMO_SHM_FLAG) != 0 &&
                 shm->magic == DEMO_SHM_MAGIC;
    int time_ok = (shm->flags & DEMO_TIME_FLAG) != 0;
    int preempt = (shm->flags & DEMO_PREEMPT_FLAG) != 0 &&
                  shm->heartbeat > 0;
    int arena_ok = (shm->flags & (DEMO_ARENA_FLAG | DEMO_ARENA_DENY_FLAG)) ==
                   (DEMO_ARENA_FLAG | DEMO_ARENA_DENY_FLAG);
    int fault_ok = g_user_faults == 1;

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
    proc_destroy(p1);
    proc_destroy(p2);
    proc_destroy(p3);
    proc_destroy(p4);
    finte_node_unref(arena);
    hal_pages_free(shm, 4096);
    finte_node_unref(chan);
    return result;
}
