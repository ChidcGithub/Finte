/* QEMU virt arm64 platform: PL011 console, generic counter, PSCI, EL1. */
#include "finte/finte.h"

#define PL011_BASE 0x09000000u
#define PL011_DR   (*(volatile uint32_t *)(PL011_BASE + 0x00))
#define PL011_FR   (*(volatile uint32_t *)(PL011_BASE + 0x18))
#define PL011_IBRD (*(volatile uint32_t *)(PL011_BASE + 0x24))
#define PL011_FBRD (*(volatile uint32_t *)(PL011_BASE + 0x28))
#define PL011_LCRH (*(volatile uint32_t *)(PL011_BASE + 0x2C))
#define PL011_CR   (*(volatile uint32_t *)(PL011_BASE + 0x30))
#define PL011_ICR  (*(volatile uint32_t *)(PL011_BASE + 0x44))

#define ARM64_HEAP_BYTES (8u * 1024u * 1024u)
#define FLOW_STACK_BYTES (32u * 1024u)
#define CTX_FRAME_BYTES 112u

typedef struct page_link {
    struct page_link *next;
} page_link_t;

static uint8_t g_heap[ARM64_HEAP_BYTES] __attribute__((aligned(4096)));
static size_t g_heap_used;
static page_link_t *g_free_pages;
static size_t g_free_page_count;
static page_link_t *g_free_stacks;
static size_t g_free_stack_count;

static fcpu_t g_cpus[FINTE_MAX_CPUS];
static fhal_ctx_t g_sched_ctx[FINTE_MAX_CPUS];
static fhal_ctx_t *g_active[FINTE_MAX_CPUS];
static uint32_t g_cpu_count = 1;
#define ARM64_AP_STACK_BYTES 16384u
uint8_t arm64_ap_stacks[FINTE_MAX_CPUS][ARM64_AP_STACK_BYTES]
    __attribute__((aligned(16)));
extern void arm64_ap_entry(void);
extern int gic_version(void);
extern void gic_send_sgi(uint32_t cpu, uint32_t irq);

static uint64_t g_freq;
static uint64_t g_console_start_ns;
static int g_console_line_start = 1;
static volatile int g_console_lock;

extern void arm64_switch(uint64_t *from_sp, uint64_t to_sp);
extern void arm64_trampoline(void);
extern char vector_table[];
extern void arm64_ap_entry(void);
extern int arm64_smp_init(void);
extern void gic_init(void);
extern void gic_init_cpu(void);
extern int gic_version(void);

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #3" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

void hal_irq_disable(void)
{
    __asm__ volatile("msr daifset, #3" : : : "memory");
}

void hal_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #3" : : : "memory");
}

uint64_t hal_irq_save(void)
{
    return irq_save();
}

void hal_irq_restore(uint64_t flags)
{
    irq_restore(flags);
}

static void uart_init(void)
{
    PL011_CR = 0;
    PL011_ICR = 0x7FF;
    PL011_IBRD = 13;
    PL011_FBRD = 1;
    PL011_LCRH = 0x70;
    PL011_CR = 0x301;
}

static void uart_putc(char c)
{
    while (PL011_FR & (1u << 5)) {
        __asm__ volatile("yield");
    }
    PL011_DR = (uint8_t)c;
}

static void console_raw(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(buf[i]);
    }
}

uint64_t hal_cycles(void)
{
    return read_cntpct();
}

uint64_t hal_cycles_per_sec(void)
{
    return g_freq;
}

uint64_t hal_now_ns(void)
{
    uint64_t freq = g_freq ? g_freq : 62500000ull;
    uint64_t count = read_cntpct();
    return (count / freq) * 1000000000ull +
           ((count % freq) * 1000000000ull) / freq;
}

static void timestamp_prefix(void)
{
    uint64_t now = hal_now_ns();
    if (now < g_console_start_ns) {
        now = g_console_start_ns;
    }
    uint64_t ns = now - g_console_start_ns;
    char buf[48];
    int n = finte_format(buf, sizeof(buf), "[%5llu.%06llu] ",
                         (unsigned long long)(ns / 1000000000ull),
                         (unsigned long long)((ns / 1000ull) % 1000000ull));
    console_raw(buf, (size_t)n);
}

void hal_console_write(const char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    uint64_t flags = irq_save();
    hal_spin_lock(&g_console_lock);
    size_t i = 0;
    while (i < len) {
        if (g_console_line_start) {
            timestamp_prefix();
            g_console_line_start = 0;
        }
        size_t j = i;
        while (j < len && buf[j] != '\n') {
            j++;
        }
        if (j < len) {
            j++;
        }
        console_raw(buf + i, j - i);
        if (j > i && buf[j - 1] == '\n') {
            g_console_line_start = 1;
        }
        i = j;
    }
    hal_spin_unlock(&g_console_lock);
    irq_restore(flags);
}

void hal_console_write_raw(const char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    uint64_t flags = irq_save();
    hal_spin_lock(&g_console_lock);
    console_raw(buf, len);
    g_console_line_start = buf[len - 1] == '\n';
    hal_spin_unlock(&g_console_lock);
    irq_restore(flags);
}

void hal_console_clear(void)
{
    hal_console_write_raw("\x1b[2J\x1b[H", 7);
}

void hal_console_clear_line(void)
{
    hal_console_write_raw("\r\x1b[K", 4);
}

int hal_console_trykey(void)
{
    if (PL011_FR & (1u << 4)) {
        return -1;
    }
    return (int)(PL011_DR & 0xFFu);
}

int hal_console_cols(void)
{
    return 80;
}

int hal_console_rows(void)
{
    return 24;
}

int hal_console_scroll(int delta)
{
    (void)delta;
    return 0;
}

int hal_console_scroll_top(void)
{
    return 0;
}

int hal_console_scroll_bottom(void)
{
    return 0;
}

int hal_console_scroll_offset(void)
{
    return 0;
}

int hal_console_trywheel(void)
{
    return -1;
}

void hal_heap_info(uint64_t *used, uint64_t *total)
{
    if (pmm_active()) {
        uint64_t pages = pmm_usable_pages();
        uint64_t free_pages = pmm_free_page_count();
        if (total) {
            *total = pages * FINTE_PAGE_SIZE;
        }
        if (used) {
            *used = (pages - free_pages) * FINTE_PAGE_SIZE;
        }
        return;
    }
    if (used) {
        *used = g_heap_used - (uint64_t)g_free_page_count * 4096ull -
                (uint64_t)g_free_stack_count * FLOW_STACK_BYTES;
    }
    if (total) {
        *total = ARM64_HEAP_BYTES;
    }
}

void *hal_pages_alloc(size_t bytes)
{
    if (bytes == 0) {
        bytes = 4096;
    }
    if (pmm_active()) {
        size_t pages = (bytes + FINTE_PAGE_SIZE - 1u) / FINTE_PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (!phys) {
            return NULL;
        }
        void *p = (void *)(uintptr_t)phys;
        finte_memset(p, 0, pages * FINTE_PAGE_SIZE);
        return p;
    }
    if (bytes == 4096 && g_free_pages) {
        uint64_t flags = irq_save();
        page_link_t *page = g_free_pages;
        g_free_pages = page->next;
        g_free_page_count--;
        irq_restore(flags);
        finte_memset(page, 0, 4096);
        return page;
    }
    if (bytes == FLOW_STACK_BYTES && g_free_stacks) {
        uint64_t flags = irq_save();
        page_link_t *block = g_free_stacks;
        g_free_stacks = block->next;
        g_free_stack_count--;
        irq_restore(flags);
        finte_memset(block, 0, bytes);
        return block;
    }
    g_heap_used = (g_heap_used + 4095u) & ~(size_t)4095u;
    if (g_heap_used + bytes > ARM64_HEAP_BYTES) {
        return NULL;
    }
    void *p = &g_heap[g_heap_used];
    g_heap_used += bytes;
    finte_memset(p, 0, bytes);
    return p;
}

void hal_pages_free(void *ptr, size_t bytes)
{
    if (!ptr || bytes == 0) {
        return;
    }
    if (pmm_active() && ((uintptr_t)ptr & 0xFFFu) == 0 &&
        pmm_owns((uint64_t)(uintptr_t)ptr, bytes)) {
        pmm_free_pages((uint64_t)(uintptr_t)ptr,
                       (bytes + FINTE_PAGE_SIZE - 1u) / FINTE_PAGE_SIZE);
        return;
    }
    if ((bytes & 0xFFFu) != 0) {
        return;
    }
    uintptr_t page = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)g_heap;
    if ((page & 0xFFFu) != 0 || page < base ||
        page + bytes > base + ARM64_HEAP_BYTES) {
        return;
    }
    uint64_t flags = irq_save();
    if (bytes == FLOW_STACK_BYTES) {
        page_link_t *link = (page_link_t *)ptr;
        link->next = g_free_stacks;
        g_free_stacks = link;
        g_free_stack_count++;
    } else {
        for (size_t off = 0; off < bytes; off += 4096u) {
            page_link_t *link = (page_link_t *)(page + off);
            link->next = g_free_pages;
            g_free_pages = link;
            g_free_page_count++;
        }
    }
    irq_restore(flags);
}

fcpu_t *hal_cpu_self(void)
{
    uint64_t self;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(self));
    if (self) {
        return (fcpu_t *)(uintptr_t)self;
    }
    return &g_cpus[0];
}

fcpu_t *hal_cpu_slot(uint32_t index)
{
    return index < FINTE_MAX_CPUS ? &g_cpus[index] : NULL;
}

uint32_t hal_cpu_count(void)
{
    return g_cpu_count;
}

void hal_cpu_install(fcpu_t *cpu)
{
    __asm__ volatile("msr tpidr_el1, %0" : : "r"((uint64_t)(uintptr_t)cpu)
                     : "memory");
}

void hal_cpu_mark_online(uint32_t index)
{
    if (index < FINTE_MAX_CPUS) {
        if (!g_cpus[index].online) {
            g_cpus[index].online = 1;
            __atomic_add_fetch(&g_cpu_count, 1, __ATOMIC_RELAXED);
        }
    }
}

void hal_spin_lock(volatile int *lock)
{
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            __asm__ volatile("yield");
        }
    }
}

void hal_spin_unlock(volatile int *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

void hal_cpu_wake(uint32_t index)
{
    if (index < FINTE_MAX_CPUS && g_cpus[index].online) {
        gic_send_sgi(index, 0);
    }
}

void hal_cpu_wake_all(void)
{
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].online) {
            gic_send_sgi(i, 0);
        }
    }
}

void hal_cpu_idle(void)
{
    /* WFI wakes on a pending physical IRQ even with DAIF masked; the
     * scheduler loop then observes g_batch and queued work. */
    __asm__ volatile("wfi");
}

void hal_timer_start_cpu(void)
{
    if (gic_version() != 2) {
        return;
    }
    uint64_t freq = hal_cycles_per_sec();
    if (!freq) {
        freq = 62500000ull;
    }
    uint64_t tval = freq / 100u;
    if (tval == 0) {
        tval = 1;
    }
    __asm__ volatile("msr cntp_ctl_el0, xzr" : : : "memory");
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(tval) : "memory");
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1ull) : "memory");
}

void hal_timer_stop_cpu(void)
{
    __asm__ volatile("msr cntp_ctl_el0, xzr" : : : "memory");
}

void hal_idle(uint64_t ns)
{
    uint64_t end = hal_now_ns() + ns;
    while (hal_now_ns() < end) {
        __asm__ volatile("yield");
    }
}

static void psci_call(uint64_t function)
{
    register uint64_t x0 __asm__("x0") = function;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
}

static uint64_t psci_call4(uint64_t function, uint64_t a1, uint64_t a2,
                           uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

static uint64_t read_mpidr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xFFull;
}

void arm64_ap_main(uint64_t index)
{
    if (index == 0 || index >= FINTE_MAX_CPUS) {
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
    fcpu_t *cpu = &g_cpus[index];
    cpu->self = cpu;
    cpu->id = (uint32_t)index;
    cpu->hw_id = (uint32_t)read_mpidr();
    cpu->timer_mode = FINTE_TIMER_ARCH;
    finte_memset(&g_sched_ctx[index], 0, sizeof(g_sched_ctx[index]));
    g_sched_ctx[index].impl = &g_sched_ctx[index];
    g_active[index] = &g_sched_ctx[index];
    hal_cpu_install(cpu);
    gic_init_cpu();
    hal_cpu_mark_online((uint32_t)index);
    finte_sched_ap_run();
}

int arm64_smp_init(void)
{
    uint32_t started = 0;
    for (uint32_t i = 1; i < FINTE_MAX_CPUS; i++) {
        uint64_t rc = psci_call4(0x84000003ull, i,
                                 (uint64_t)(uintptr_t)arm64_ap_entry, i);
        if (rc != 0) {
            break;
        }
        started++;
    }
    uint64_t deadline = hal_now_ns() + 200000000ull;
    while (g_cpu_count < 1u + started && hal_now_ns() < deadline) {
        __asm__ volatile("yield");
    }
    return (int)started;
}

void hal_exit(int code)
{
    (void)code;
    psci_call(0x84000008ull); /* PSCI SYSTEM_OFF */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void hal_panic(const char *msg)
{
    hal_console_write("\n[kernel] PANIC: ", 17);
    hal_console_write(msg ? msg : "unknown", finte_strlen(msg ? msg : "unknown"));
    hal_console_write("\n", 1);
    hal_exit(0x7F);
}

int hal_fw_begin(void)
{
    return 0;
}

void hal_fw_end(void)
{
}

int hal_blk_count(void)
{
    return 0;
}

int hal_blk_info(int index, fblk_info_t *out)
{
    (void)index;
    (void)out;
    return -1;
}

int hal_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors)
{
    (void)index;
    (void)lba;
    (void)buf;
    (void)sectors;
    return -1;
}

int hal_blk_write(int index, uint64_t lba, const void *buf, uint32_t sectors)
{
    (void)index;
    (void)lba;
    (void)buf;
    (void)sectors;
    return -1;
}

static void arm64_set_addr_space(uint64_t root)
{
    __asm__ volatile("msr ttbr0_el1, %0; dsb ish; tlbi vmalle1; isb"
                     : : "r"(root) : "memory");
}

fhal_ctx_t *hal_ctx_self(void)
{
    uint32_t id = hal_cpu_self()->id;
    if (id >= FINTE_MAX_CPUS) {
        id = 0;
    }
    return g_active[id] ? g_active[id] : &g_sched_ctx[id];
}

fhal_ctx_t *hal_ctx_create(void (*entry)(void *), void *arg, size_t stack_bytes)
{
    if (stack_bytes < 16384) {
        stack_bytes = 16384;
    }
    stack_bytes = (stack_bytes + 4095u) & ~(size_t)4095u;
    fhal_ctx_t *ctx = (fhal_ctx_t *)hal_pages_alloc(4096);
    if (!ctx) {
        return NULL;
    }
    void *stack = hal_pages_alloc(stack_bytes);
    if (!stack) {
        hal_pages_free(ctx, 4096);
        return NULL;
    }
    ctx->entry = entry;
    ctx->arg = arg;
    ctx->impl = ctx;
    ctx->stack = stack;
    ctx->stack_size = stack_bytes;
    ctx->addr_space = 0;

    uintptr_t top = ((uintptr_t)stack + stack_bytes) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - CTX_FRAME_BYTES);
    for (uint32_t i = 0; i < CTX_FRAME_BYTES / 8; i++) {
        frame[i] = 0;
    }
    frame[10] = 0; /* x29 */
    frame[11] = (uint64_t)(uintptr_t)arm64_trampoline; /* x30 */
    frame[12] = 0; /* DAIF: unmask debug/SError/IRQ/FIQ */
    ctx->sp = (uint64_t)(uintptr_t)frame;
    return ctx;
}

void hal_ctx_destroy(fhal_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    for (uint32_t i = 0; i < FINTE_MAX_CPUS; i++) {
        if (ctx == &g_sched_ctx[i]) {
            return;
        }
    }
    void *stack = ctx->stack;
    size_t bytes = ctx->stack_size;
    hal_pages_free(ctx, 4096);
    hal_pages_free(stack, bytes);
}

void hal_ctx_switch(fhal_ctx_t *from, fhal_ctx_t *to)
{
    if (!from || !to) {
        hal_panic("bad context switch");
    }
    if (to->addr_space) {
        arm64_set_addr_space(to->addr_space);
    }
    uint32_t id = hal_cpu_self()->id;
    if (id < FINTE_MAX_CPUS) {
        g_active[id] = to;
    }
    arm64_switch(&from->sp, to->sp);
}

void finte_hal_bootstrap(void)
{
    fhal_ctx_t *ctx = hal_ctx_self();
    ctx->entry(ctx->arg);
    hal_panic("flow entry returned");
}

void hal_init(void)
{
    uint64_t flags = irq_save();
    uart_init();
    g_freq = read_cntfrq();
    if (g_freq == 0) {
        g_freq = 62500000ull;
    }
    g_console_start_ns = hal_now_ns();
    g_console_line_start = 1;
    finte_memset(g_cpus, 0, sizeof(g_cpus));
    finte_memset(g_sched_ctx, 0, sizeof(g_sched_ctx));
    finte_memset(g_active, 0, sizeof(g_active));
    g_cpu_count = 1;
    g_cpus[0].self = &g_cpus[0];
    g_cpus[0].id = 0;
    g_cpus[0].hw_id = 0;
    g_cpus[0].online = 1;
    g_cpus[0].timer_mode = FINTE_TIMER_ARCH;
    g_sched_ctx[0].entry = NULL;
    g_sched_ctx[0].arg = NULL;
    g_sched_ctx[0].impl = &g_sched_ctx[0];
    g_active[0] = &g_sched_ctx[0];
    hal_cpu_install(&g_cpus[0]);
    __asm__ volatile("msr vbar_el1, %0; isb" : : "r"(vector_table) : "memory");
    gic_init();
    gic_init_cpu();
    irq_restore(flags);
}

void arm64_fault(void)
{
    uint64_t esr;
    uint64_t far;
    uint64_t elr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    char buf[192];
    int n = finte_format(buf, sizeof(buf),
                         "\n[arm64] fault esr=0x%llx far=0x%llx elr=0x%llx\n",
                         (unsigned long long)esr,
                         (unsigned long long)far,
                         (unsigned long long)elr);
    hal_console_write_raw(buf, (size_t)n);
    hal_exit(0x71);
}
