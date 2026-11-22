#include "finte/finte.h"
#include "hal_x86.h"
#include "vga.h"
#include "fb.h"
#include "input.h"
#include "scroll.h"

#define COM1 0x3F8
#define KERNEL_HEAP_BYTES (32u * 1024u * 1024u)
#define KERNEL_TSC_HZ 1000000000ull

static fhal_ctx_t g_sched[FINTE_MAX_CPUS];
static fhal_ctx_t *g_active[FINTE_MAX_CPUS];
static fcpu_t g_cpus[FINTE_MAX_CPUS];
static uint32_t g_cpu_count = 1;
static int g_cpu_gs_ready;
static const fboot_info_t *g_boot;
static int g_uefi;
static uint8_t g_heap[KERNEL_HEAP_BYTES] __attribute__((aligned(4096)));
static size_t g_heap_used;

static uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

typedef struct page_link {
    struct page_link *next;
} page_link_t;

static page_link_t *g_free_pages;
static size_t g_free_page_count;
static page_link_t *g_free_stacks;
static size_t g_free_stack_count;

#define CPU_PAGE_CACHE 4
static void *g_cpu_pages[FINTE_MAX_CPUS][CPU_PAGE_CACHE];
static uint8_t g_cpu_page_count[FINTE_MAX_CPUS];
static void *g_cpu_stacks[FINTE_MAX_CPUS];
static size_t g_cpu_cached_pages;
static size_t g_cpu_cached_stacks;

#define FLOW_STACK_BYTES (32u * 1024u)

static uint32_t page_cpu(void)
{
    uint32_t id = hal_cpu_self()->id;
    return id < FINTE_MAX_CPUS ? id : 0;
}

static uint64_t g_fw_idt_base;
static uint32_t g_fw_idt_limit;
static int g_fw_active;
static uint64_t g_fw_flags;

typedef struct __attribute__((packed)) idt_ptr_priv {
    uint16_t limit;
    uint64_t base;
} idt_ptr_priv_t;

static idt_ptr_priv_t g_saved_idt;

static uint64_t g_tsc_hz = 1000000000ull;
static uint64_t g_console_start_ns;
static int g_console_line_start = 1;
static volatile int g_console_lock;

static void calibrate_tsc(void);
static uint64_t cycles_to_ns(uint64_t cycles);
static void console_emit(const char *buf, size_t len);

extern void kx_switch(uint64_t *from_rsp, uint64_t to_rsp);
extern void kx_trampoline(void);

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (uint8_t)c);
}

static void serial_write_raw(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            serial_putc('\r');
        }
        serial_putc(buf[i]);
    }
}

static void console_emit(const char *buf, size_t len)
{
    serial_write_raw(buf, len);
    int scrolled = sb_offset() > 0;
    sb_write(buf, len);
    if (scrolled) {
        /* The screen currently shows history; keep it stable. */
        return;
    }
    if (g_uefi) {
        fb_write(buf, len);
    } else {
        vga_write(buf, len);
    }
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
    console_emit(buf, (size_t)n);
}

void hal_boot_info(const fboot_info_t *bi)
{
    g_boot = bi;
    g_uefi = bi && bi->magic == FINTE_BOOT_MAGIC &&
             bi->console_type == FINTE_BOOT_CONSOLE_UEFI;
    int fw = g_uefi && bi->fw_services;
    g_fw_idt_base = fw ? bi->fw_idt_base : 0;
    g_fw_idt_limit = fw ? bi->fw_idt_limit : 0;
}

int hal_fw_begin(void)
{
    if (!g_uefi || !g_fw_idt_base || g_fw_active) {
        return 0;
    }
    __asm__ volatile("pushfq; pop %0" : "=r"(g_fw_flags));
    __asm__ volatile("cli");
    __asm__ volatile("sidt %0" : "=m"(g_saved_idt));
    idt_ptr_priv_t fw = { (uint16_t)g_fw_idt_limit, g_fw_idt_base };
    __asm__ volatile("lidt %0" : : "m"(fw));
    __asm__ volatile("sti");
    g_fw_active = 1;
    return 1;
}

void hal_fw_end(void)
{
    if (!g_fw_active) {
        return;
    }
    __asm__ volatile("cli");
    __asm__ volatile("lidt %0" : : "m"(g_saved_idt));
    g_fw_active = 0;
    if (g_fw_flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

void hal_irq_disable(void)
{
    __asm__ volatile("cli");
}

void hal_irq_enable(void)
{
    __asm__ volatile("sti");
}

uint64_t hal_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void hal_irq_restore(uint64_t flags)
{
    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

void hal_console_write(const char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    uint64_t flags = hal_irq_save();
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
            j++; /* include the newline */
        }
        console_emit(buf + i, j - i);
        if (j > i && buf[j - 1] == '\n') {
            g_console_line_start = 1;
        }
        i = j;
    }
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
}

/* Prompt and live input text: no timestamp, so redraws are stable. */
void hal_console_write_raw(const char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_console_lock);
    console_emit(buf, len);
    if (buf[len - 1] == '\n') {
        g_console_line_start = 1;
    } else {
        g_console_line_start = 0;
    }
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
}

void hal_console_clear(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_console_lock);
    g_console_line_start = 1;
    sb_clear();
    serial_write_raw("\033[2J\033[H", 7);
    if (g_uefi) {
        fb_clear();
    } else {
        vga_clear();
    }
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
}

void hal_console_clear_line(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_console_lock);
    int scrolled = sb_offset() > 0;
    sb_clear_line();
    serial_write_raw("\r\033[K", 4);
    if (!scrolled) {
        if (g_uefi) {
            fb_clear_line();
        } else {
            vga_clear_line();
        }
    }
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
}

int hal_console_trykey(void)
{
    /* Serial and PS/2 polling remain valid after handoff. UEFI ConIn is not
     * called from the kernel because firmware console paths may depend on
     * events that are unavailable after the entry path disables interrupts. */
    return input_trykey();
}

int hal_console_cols(void)
{
    return sb_cols();
}

int hal_console_rows(void)
{
    return sb_rows();
}

int hal_console_scroll(int delta)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_console_lock);
    int offset = sb_scroll(delta);
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
    return offset;
}

int hal_console_scroll_top(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_console_lock);
    int offset = sb_scroll_top();
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
    return offset;
}

int hal_console_scroll_bottom(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_console_lock);
    int offset = sb_scroll_bottom();
    hal_spin_unlock(&g_console_lock);
    hal_irq_restore(flags);
    return offset;
}

int hal_console_scroll_offset(void)
{
    return sb_offset();
}

int hal_console_trywheel(void)
{
    return input_trywheel();
}

void hal_heap_info(uint64_t *used, uint64_t *total)
{
    if (used) {
        *used = g_heap_used - (uint64_t)g_free_page_count * 4096ull -
                (uint64_t)g_free_stack_count * FLOW_STACK_BYTES -
                (uint64_t)g_cpu_cached_pages * 4096ull -
                (uint64_t)g_cpu_cached_stacks * FLOW_STACK_BYTES;
    }
    if (total) {
        *total = KERNEL_HEAP_BYTES;
    }
}

void hal_init(void)
{
    uint64_t start_cycles = hal_cycles();
    calibrate_tsc();
    g_console_start_ns = cycles_to_ns(start_cycles);
    g_console_line_start = 1;

    serial_init();
    if (g_uefi) {
        if (g_boot) {
            fb_init(g_boot);
        }
        if (fb_ready()) {
            sb_init(fb_text_cols(), fb_text_rows(), 1);
        } else {
            sb_init(80, 25, 1);
        }
    } else {
        vga_init();
        sb_init(vga_cols(), vga_rows(), 0);
    }
    input_init();
    g_cpus[0].self = &g_cpus[0];
    g_cpus[0].id = 0;
    g_cpus[0].online = 1;
    g_cpus[0].timer_mode = FINTE_TIMER_NONE;
    g_cpu_count = 1;
    hal_cpu_install(&g_cpus[0]);
    g_active[0] = &g_sched[0];
    g_sched[0].impl = &g_sched[0];
    g_heap_used = 0;
    g_free_pages = NULL;
    g_free_page_count = 0;
    g_free_stacks = NULL;
    g_free_stack_count = 0;
    g_cpu_cached_pages = 0;
    g_cpu_cached_stacks = 0;
    finte_memset(g_cpu_page_count, 0, sizeof(g_cpu_page_count));
    finte_memset(g_cpu_stacks, 0, sizeof(g_cpu_stacks));
}

fcpu_t *hal_cpu_self(void)
{
    if (!g_cpu_gs_ready) {
        return &g_cpus[0];
    }
    fcpu_t *cpu = NULL;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(cpu));
    if (!cpu) {
        cpu = &g_cpus[0];
    }
    return cpu;
}

fcpu_t *hal_cpu_slot(uint32_t index)
{
    if (index >= FINTE_MAX_CPUS) {
        return NULL;
    }
    return &g_cpus[index];
}

uint32_t hal_cpu_count(void)
{
    return g_cpu_count;
}

void hal_cpu_install(fcpu_t *cpu)
{
    if (!cpu) {
        return;
    }
    cpu->self = cpu;
    uint64_t base = (uint64_t)(uintptr_t)cpu;
    uint32_t lo = (uint32_t)base;
    uint32_t hi = (uint32_t)(base >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000101u), "a"(lo), "d"(hi));
    g_cpu_gs_ready = 1;
}

void hal_cpu_mark_online(uint32_t index)
{
    if (index >= FINTE_MAX_CPUS) {
        return;
    }
    g_cpus[index].online = 1;
    uint32_t want = index + 1;
    uint32_t cur = __atomic_load_n(&g_cpu_count, __ATOMIC_RELAXED);
    while (want > cur) {
        if (__atomic_compare_exchange_n(&g_cpu_count, &cur, want, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }
}

void hal_spin_lock(volatile int *lock)
{
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            __asm__ volatile("pause");
        }
    }
}

void hal_spin_unlock(volatile int *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

uint64_t hal_cycles(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t hal_cycles_per_sec(void)
{
    return g_tsc_hz;
}

static uint64_t cycles_to_ns(uint64_t cycles)
{
    uint64_t hz = g_tsc_hz ? g_tsc_hz : KERNEL_TSC_HZ;
    return (cycles / hz) * 1000000000ull +
           ((cycles % hz) * 1000000000ull) / hz;
}

uint64_t hal_now_ns(void)
{
    return cycles_to_ns(hal_cycles());
}

/* Measure the TSC with PIT channel 2 over ~10 ms. This keeps timestamps and
 * sleep deadlines meaningful on hardware whose TSC is not 1 GHz. */
static void calibrate_tsc(void)
{
    const uint16_t pit_divisor = 11932;
    uint8_t p61 = inb(0x61);

    /* Gate low, speaker off; channel 2, lo/hi, mode 0. */
    outb(0x61, (uint8_t)(p61 & 0xFC));
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)(pit_divisor & 0xFF));
    outb(0x42, (uint8_t)(pit_divisor >> 8));
    outb(0x61, (uint8_t)((p61 & 0xFC) | 0x01)); /* gate high */

    uint64_t t0 = hal_cycles();
    uint64_t limit = t0 + 500000000ull; /* ~0.5 s at 1 GHz */
    int done = 0;
    while ((inb(0x61) & 0x20) == 0) {
        if (hal_cycles() > limit) {
            break;
        }
    }
    done = (inb(0x61) & 0x20) != 0;
    uint64_t t1 = hal_cycles();
    outb(0x61, (uint8_t)(p61 & 0xFC));

    if (done && t1 > t0) {
        uint64_t hz = ((t1 - t0) * 1193182ull) / pit_divisor;
        if (hz >= 100000000ull && hz <= 10000000000ull) {
            g_tsc_hz = hz;
        }
    }
}

void hal_idle(uint64_t ns)
{
    uint64_t deadline = hal_now_ns() + ns;
    while (hal_now_ns() < deadline) {
        __asm__ volatile("pause");
    }
}

void hal_exit(int code)
{
    (void)code;
    /* isa-debug-exit: QEMU exits with (value << 1) | 1. */
    outb(0xF4, 0x10);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void hal_panic(const char *msg)
{
    hal_console_write("\n[kernel] PANIC: ", 16);
    hal_console_write(msg ? msg : "unknown", finte_strlen(msg ? msg : "unknown"));
    hal_console_write("\n", 1);
    outb(0xF4, 0x7F);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

int hal_is_uefi(void)
{
    return g_uefi;
}

void *hal_pages_alloc(size_t bytes)
{
    if (bytes == 0) {
        bytes = 4096;
    }
    uint32_t id = page_cpu();
    if (bytes == 4096 && g_cpu_page_count[id]) {
        void *page = g_cpu_pages[id][--g_cpu_page_count[id]];
        g_cpu_cached_pages--;
        finte_memset(page, 0, 4096);
        return page;
    }
    if (bytes == FLOW_STACK_BYTES && g_cpu_stacks[id]) {
        void *stack = g_cpu_stacks[id];
        g_cpu_stacks[id] = NULL;
        g_cpu_cached_stacks--;
        finte_memset(stack, 0, bytes);
        return stack;
    }
    if (bytes == FLOW_STACK_BYTES) {
        uint64_t flags = hal_irq_save();
        page_link_t *block = g_free_stacks;
        if (block) {
            g_free_stacks = block->next;
            g_free_stack_count--;
            hal_irq_restore(flags);
            finte_memset(block, 0, bytes);
            return block;
        }
        hal_irq_restore(flags);
    }
    if (bytes == 4096) {
        uint64_t flags = hal_irq_save();
        page_link_t *page = g_free_pages;
        if (page) {
            g_free_pages = page->next;
            g_free_page_count--;
            hal_irq_restore(flags);
            finte_memset(page, 0, 4096);
            return page;
        }
        hal_irq_restore(flags);
    }
    g_heap_used = (g_heap_used + 4095u) & ~(size_t)4095u;
    if (g_heap_used + bytes > KERNEL_HEAP_BYTES) {
        return NULL;
    }
    void *p = &g_heap[g_heap_used];
    g_heap_used += bytes;
    finte_memset(p, 0, bytes);
    return p;
}

void hal_pages_free(void *ptr, size_t bytes)
{
    if (!ptr || bytes == 0 || (bytes & 0xFFFu) != 0) {
        return;
    }
    uintptr_t page = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)g_heap;
    if ((page & 0xFFFu) != 0 || page < base ||
        page + bytes > base + KERNEL_HEAP_BYTES) {
        return;
    }
    uint32_t id = page_cpu();
    if (bytes == 4096 && g_cpu_page_count[id] < CPU_PAGE_CACHE) {
        g_cpu_pages[id][g_cpu_page_count[id]++] = ptr;
        g_cpu_cached_pages++;
        return;
    }
    if (bytes == FLOW_STACK_BYTES && !g_cpu_stacks[id]) {
        g_cpu_stacks[id] = ptr;
        g_cpu_cached_stacks++;
        return;
    }
    uint64_t flags = hal_irq_save();
    for (size_t off = 0; off < bytes; off += 4096u) {
        page_link_t *link = (page_link_t *)(page + off);
        link->next = g_free_pages;
        g_free_pages = link;
        g_free_page_count++;
    }
    hal_irq_restore(flags);
}

fhal_ctx_t *hal_ctx_self(void)
{
    fcpu_t *cpu = hal_cpu_self();
    uint32_t id = cpu->id < FINTE_MAX_CPUS ? cpu->id : 0;
    if (!g_active[id]) {
        g_active[id] = &g_sched[id];
    }
    if (g_sched[id].addr_space == 0) {
        g_sched[id].addr_space = read_cr3();
    }
    return g_active[id];
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
    void *stack = NULL;
    if (stack_bytes == FLOW_STACK_BYTES && g_free_stacks) {
        uint64_t flags = hal_irq_save();
        page_link_t *block = g_free_stacks;
        if (block) {
            g_free_stacks = block->next;
            g_free_stack_count--;
            hal_irq_restore(flags);
            stack = block;
            finte_memset(stack, 0, stack_bytes);
        } else {
            hal_irq_restore(flags);
        }
    }
    if (!stack) {
        stack = hal_pages_alloc(stack_bytes);
    }
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
    uint64_t *sp = (uint64_t *)(top - 64);
    sp[0] = 0x202;           /* RFLAGS: IF=1, reserved bit 1 */
    sp[1] = 0;
    sp[2] = 0;
    sp[3] = 0;
    sp[4] = 0;
    sp[5] = 0;
    sp[6] = 0;
    sp[7] = (uint64_t)(uintptr_t)kx_trampoline;
    ctx->sp = (uint64_t)(uintptr_t)sp;
    return ctx;
}

void hal_ctx_destroy(fhal_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    for (uint32_t i = 0; i < FINTE_MAX_CPUS; i++) {
        if (ctx == &g_sched[i]) {
            return;
        }
    }
    void *stack = ctx->stack;
    size_t bytes = ctx->stack_size;
    hal_pages_free(ctx, 4096);
    if (stack && bytes == FLOW_STACK_BYTES) {
        uint64_t flags = hal_irq_save();
        page_link_t *link = (page_link_t *)stack;
        link->next = g_free_stacks;
        g_free_stacks = link;
        g_free_stack_count++;
        hal_irq_restore(flags);
        return;
    }
    for (size_t off = 0; stack && off < bytes; off += 4096u) {
        hal_pages_free((uint8_t *)stack + off, 4096);
    }
}

void hal_ctx_switch(fhal_ctx_t *from, fhal_ctx_t *to)
{
    if (!from || !to) {
        hal_panic("bad context switch");
    }
    fcpu_t *cpu = hal_cpu_self();
    uint32_t id = cpu->id < FINTE_MAX_CPUS ? cpu->id : 0;
    g_active[id] = to;
    if (to->stack && to->stack_size && to->addr_space) {
        hal_user_bind_stack((uint8_t *)to->stack + to->stack_size);
    }
    if (to->addr_space) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(to->addr_space) : "memory");
    }
    kx_switch(&from->sp, to->sp);
}

void finte_hal_bootstrap(void)
{
    fhal_ctx_t *ctx = hal_ctx_self();
    ctx->entry(ctx->arg);
    hal_panic("flow entry returned");
}
