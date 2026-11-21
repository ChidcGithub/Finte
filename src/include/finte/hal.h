/* Platform boundary.
 *
 * The core never touches hardware directly. A platform provides console,
 * time, CPU-local state, page allocation, contexts and block devices. Host
 * runs use ucontext/fibers; bare-metal ports provide their own context switch.
 */
#ifndef FINTE_HAL_H
#define FINTE_HAL_H

#include "finte/base.h"

typedef struct fhal_ctx fhal_ctx_t;

#define FINTE_MAX_CPUS 8

typedef enum {
    FINTE_TIMER_NONE = 0,
    FINTE_TIMER_ONESHOT = 1,
    FINTE_TIMER_PERIODIC = 2,
    FINTE_TIMER_ARCH = 3
} fhal_timer_mode_t;

/* Per-CPU scheduling and interrupt state. Platforms that use a CPU-local
 * register may keep `self` first. */
typedef struct fcpu {
    struct fcpu *self;
    struct fflow *current;
    struct fhal_ctx *sched_ctx;
    uint32_t id;
    uint32_t hw_id;             /* APIC id, MPIDR affinity, ... */
    volatile uint32_t online;
    volatile uint32_t timer_mode; /* fhal_timer_mode_t */
    volatile uint32_t need_resched;
    volatile uint32_t idle;
    uint32_t core_depth;
    uint32_t timer_period;
    uint64_t lock_flags;
    uint64_t ticks;
    uint64_t switches;
    uint64_t ipis;
    struct fflow *runq[2];
    struct fflow *runq_tail[2];
} fcpu_t;

struct fhal_ctx {
    void (*entry)(void *arg);
    void *arg;
    void *impl;          /* platform-specific handle */
    void *stack;
    size_t stack_size;
    uint64_t addr_space; /* platform page-table root, 0 = keep current */
    uint64_t sp;         /* platform context stack pointer */
};

void hal_init(void);
void hal_console_write(const char *buf, size_t len);
void hal_console_write_raw(const char *buf, size_t len);
void hal_console_clear(void);
void hal_console_clear_line(void);
int hal_console_trykey(void);
int hal_console_cols(void);
int hal_console_rows(void);
int hal_console_scroll(int delta);
int hal_console_scroll_top(void);
int hal_console_scroll_bottom(void);
int hal_console_scroll_offset(void);
int hal_console_trywheel(void);
void hal_heap_info(uint64_t *used, uint64_t *total);
uint64_t hal_cycles(void);
uint64_t hal_cycles_per_sec(void);
uint64_t hal_now_ns(void);
void hal_idle(uint64_t ns);
void hal_exit(int code) __attribute__((noreturn));
void hal_panic(const char *msg) __attribute__((noreturn));

/* Three-strike system watchdog. */
void hal_watchdog_strike(const char *reason);
void hal_watchdog_suspend(void);
void hal_watchdog_resume(void);
int hal_watchdog_strikes(void);
const char *hal_watchdog_last(void);
void hal_watchdog_clear(void);

/* CPU-local state and SMP primitives. */
fcpu_t *hal_cpu_self(void);
fcpu_t *hal_cpu_slot(uint32_t index);
uint32_t hal_cpu_count(void);
void hal_cpu_install(fcpu_t *cpu);
void hal_cpu_mark_online(uint32_t index);
void hal_spin_lock(volatile int *lock);
void hal_spin_unlock(volatile int *lock);
void hal_cpu_wake(uint32_t index);
void hal_cpu_wake_all(void);
void hal_cpu_idle(void);
void hal_timer_start_cpu(void);
void hal_timer_stop_cpu(void);

/* Raw block devices. The disk manager works only through this interface. */
#define FBLK_NAME_MAX 16
#define FBLK_MAX 8
#define FBLK_FLAG_REMOVABLE 1u

typedef struct fblk_info {
    char name[FBLK_NAME_MAX];
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t flags;
} fblk_info_t;

int hal_blk_count(void);
int hal_blk_info(int index, fblk_info_t *out);
int hal_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors);
int hal_blk_write(int index, uint64_t lba, const void *buf, uint32_t sectors);

/* Temporarily switch to the firmware IDT for UEFI Boot Services calls. */
int hal_fw_begin(void);
void hal_fw_end(void);

/* Interrupt-state control for the scheduler copy of IF. */
void hal_irq_disable(void);
void hal_irq_enable(void);
uint64_t hal_irq_save(void);
void hal_irq_restore(uint64_t flags);

/* Page-granular allocation used for arenas and stacks. */
void *hal_pages_alloc(size_t bytes);
void hal_pages_free(void *ptr, size_t bytes);

fhal_ctx_t *hal_ctx_self(void);
fhal_ctx_t *hal_ctx_create(void (*entry)(void *), void *arg, size_t stack_bytes);
void hal_ctx_destroy(fhal_ctx_t *ctx);
void hal_ctx_switch(fhal_ctx_t *from, fhal_ctx_t *to);

/* Called by the tiny platform trampoline with the active context installed. */
void finte_hal_bootstrap(void);

#endif /* FINTE_HAL_H */
