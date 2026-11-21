#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#endif

#include "finte/finte.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>
#include <unistd.h>
#endif

#define HOST_STACK (64u * 1024u)

static fhal_ctx_t *g_active;
static fcpu_t g_host_cpu;

fcpu_t *hal_cpu_self(void)
{
    g_host_cpu.self = &g_host_cpu;
    g_host_cpu.online = 1;
    return &g_host_cpu;
}

fcpu_t *hal_cpu_slot(uint32_t index)
{
    return index == 0 ? &g_host_cpu : NULL;
}

uint32_t hal_cpu_count(void)
{
    return 1;
}

void hal_cpu_install(fcpu_t *cpu)
{
    (void)cpu;
}

void hal_cpu_mark_online(uint32_t index)
{
    (void)index;
}

void hal_spin_lock(volatile int *lock)
{
    *lock = 1;
}

void hal_spin_unlock(volatile int *lock)
{
    *lock = 0;
}

void hal_cpu_wake(uint32_t index)
{
    (void)index;
}

void hal_cpu_wake_all(void)
{
}

void hal_cpu_idle(void)
{
}

void hal_timer_start_cpu(void)
{
}

void hal_timer_stop_cpu(void)
{
}

#if defined(_WIN32)
typedef struct host_ctx {
    fhal_ctx_t pub;
    LPVOID fiber;
} host_ctx_t;

static host_ctx_t g_sched;
static LARGE_INTEGER g_freq;

static void CALLBACK host_fiber_entry(void *arg)
{
    (void)arg;
    finte_hal_bootstrap();
}
#else
typedef struct host_ctx {
    fhal_ctx_t pub;
    ucontext_t uc;
} host_ctx_t;

static host_ctx_t g_sched;

static void host_trampoline(void)
{
    finte_hal_bootstrap();
}
#endif

void finte_hal_bootstrap(void)
{
    fhal_ctx_t *c = hal_ctx_self();
    c->entry(c->arg);
    hal_panic("flow entry returned");
}

void hal_init(void)
{
#if defined(_WIN32)
    QueryPerformanceFrequency(&g_freq);
#endif
    g_active = NULL;
}

void hal_boot_info(const fboot_info_t *bi)
{
    (void)bi;
}

int hal_fw_begin(void)
{
    return 0;
}

void hal_fw_end(void)
{
}

void hal_irq_disable(void)
{
}

void hal_irq_enable(void)
{
}

uint64_t hal_irq_save(void)
{
    return 0x200;
}

void hal_irq_restore(uint64_t flags)
{
    (void)flags;
}

void hal_console_write(const char *buf, size_t len)
{
    if (len) {
        fwrite(buf, 1, len, stdout);
        fflush(stdout);
    }
}

void hal_console_write_raw(const char *buf, size_t len)
{
    hal_console_write(buf, len);
}

void hal_console_clear(void)
{
    fputs("\033[2J\033[H", stdout);
    fflush(stdout);
}

void hal_console_clear_line(void)
{
    fputs("\r\033[K", stdout);
    fflush(stdout);
}

int hal_console_trykey(void)
{
    return -1;
}

void hal_heap_info(uint64_t *used, uint64_t *total)
{
    if (used) {
        *used = 0;
    }
    if (total) {
        *total = 0;
    }
}

uint64_t hal_cycles(void)
{
#if defined(_WIN32)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t hal_cycles_per_sec(void)
{
#if defined(_WIN32)
    return (uint64_t)g_freq.QuadPart;
#else
    return 1000000000ull;
#endif
}

uint64_t hal_now_ns(void)
{
    return (uint64_t)((long double)hal_cycles() * 1000000000.0L /
                      (long double)hal_cycles_per_sec());
}

void hal_idle(uint64_t ns)
{
#if defined(_WIN32)
    DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
    Sleep(ms ? ms : 1);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    nanosleep(&ts, NULL);
#endif
}

void hal_exit(int code)
{
    exit(code);
}

void hal_panic(const char *msg)
{
    hal_console_write("\nfinte panic: ", 14);
    hal_console_write(msg ? msg : "unknown", msg ? strlen(msg) : 7);
    hal_console_write("\n", 1);
    exit(70);
}

void hal_watchdog_strike(const char *reason)
{
    (void)reason;
}

void hal_watchdog_suspend(void)
{
}

void hal_watchdog_resume(void)
{
}

int hal_watchdog_strikes(void)
{
    return 0;
}

const char *hal_watchdog_last(void)
{
    return "";
}

void hal_watchdog_clear(void)
{
}

int hal_is_uefi(void)
{
    return 0;
}

void *hal_pages_alloc(size_t bytes)
{
    size_t total = bytes + 4096 + 32;
    void *raw = malloc(total);
    if (!raw) {
        return NULL;
    }
    uintptr_t a = ((uintptr_t)raw + 32 + 4095u) & ~(uintptr_t)4095u;
    ((void **)a)[-1] = raw;
    memset((void *)a, 0, bytes);
    return (void *)a;
}

void hal_pages_free(void *ptr, size_t bytes)
{
    (void)bytes;
    if (!ptr) {
        return;
    }
    void *raw = ((void **)ptr)[-1];
    free(raw);
}

fhal_ctx_t *hal_ctx_self(void)
{
    if (!g_active) {
        memset(&g_sched, 0, sizeof(g_sched));
#if defined(_WIN32)
        g_sched.pub.impl = &g_sched;
        g_sched.fiber = ConvertThreadToFiber(NULL);
        if (!g_sched.fiber) {
            hal_panic("ConvertThreadToFiber failed");
        }
#else
        g_sched.pub.impl = &g_sched;
        if (getcontext(&g_sched.uc) != 0) {
            hal_panic("getcontext failed");
        }
#endif
        g_active = &g_sched.pub;
    }
    return g_active;
}

fhal_ctx_t *hal_ctx_create(void (*entry)(void *), void *arg, size_t stack_bytes)
{
    if (stack_bytes < 16384) {
        stack_bytes = 16384;
    }
    host_ctx_t *hc = (host_ctx_t *)calloc(1, sizeof(*hc));
    if (!hc) {
        return NULL;
    }
    hc->pub.entry = entry;
    hc->pub.arg = arg;
    hc->pub.impl = hc;
    hc->pub.stack_size = stack_bytes;
    hc->pub.stack = malloc(stack_bytes);
    if (!hc->pub.stack) {
        free(hc);
        return NULL;
    }
#if defined(_WIN32)
    hc->fiber = CreateFiber((SIZE_T)stack_bytes, host_fiber_entry, hc);
    if (!hc->fiber) {
        free(hc->pub.stack);
        free(hc);
        return NULL;
    }
#else
    if (getcontext(&hc->uc) != 0) {
        free(hc->pub.stack);
        free(hc);
        return NULL;
    }
    hc->uc.uc_stack.ss_sp = hc->pub.stack;
    hc->uc.uc_stack.ss_size = stack_bytes;
    hc->uc.uc_link = NULL;
    makecontext(&hc->uc, host_trampoline, 0);
#endif
    return &hc->pub;
}

void hal_ctx_destroy(fhal_ctx_t *ctx)
{
    if (!ctx || ctx == &g_sched.pub) {
        return;
    }
    host_ctx_t *hc = (host_ctx_t *)ctx->impl;
    if (!hc) {
        return;
    }
#if defined(_WIN32)
    if (hc->fiber) {
        DeleteFiber(hc->fiber);
    }
#endif
    free(hc->pub.stack);
    free(hc);
}

void hal_ctx_switch(fhal_ctx_t *from, fhal_ctx_t *to)
{
    if (!from || !to) {
        hal_panic("bad context switch");
    }
    host_ctx_t *hf = (host_ctx_t *)from->impl;
    host_ctx_t *ht = (host_ctx_t *)to->impl;
    if (!hf || !ht) {
        hal_panic("bad context");
    }
    g_active = to;
#if defined(_WIN32)
    (void)hf;
    SwitchToFiber(ht->fiber);
#else
    swapcontext(&hf->uc, &ht->uc);
#endif
}
