/* Ring-3 GDT/TSS and the int 0x80 syscall gate.
 *
 * Address spaces and ELF loading live in process.c.  This file only owns the
 * privileged transition state: the user GDT descriptors, the TSS (whose RSP0
 * follows the running process's kernel stack) and the syscall entry. */
#include "finte/finte.h"
#include "process.h"
#include "hal_x86.h"

typedef struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

typedef struct __attribute__((packed)) gdtr64 {
    uint16_t limit;
    uint64_t base;
} gdtr64_t;

static uint64_t g_gdt[FINTE_MAX_CPUS][7] __attribute__((aligned(16)));
static tss64_t g_tss[FINTE_MAX_CPUS] __attribute__((aligned(16)));
static uint8_t g_gdt_ready[FINTE_MAX_CPUS];

extern void um_load_gdt(const gdtr64_t *g);
extern void um_enter(uint64_t rip, uint64_t rsp, uint64_t flags)
    __attribute__((noreturn));
extern void faults_set_kernel_cs(uint16_t cs);

static void gdt_set(uint64_t *gdt, int index, uint64_t value)
{
    gdt[index] = value;
}

static void build_gdt(uint32_t cpu)
{
    if (cpu >= FINTE_MAX_CPUS) {
        return;
    }
    uint64_t *gdt = g_gdt[cpu];
    tss64_t *tss = &g_tss[cpu];
    uint64_t base = (uint64_t)(uintptr_t)tss;
    uint64_t limit = sizeof(*tss) - 1;
    gdt_set(gdt, 0, 0);
    gdt_set(gdt, 1, 0x00AF9A000000FFFFull); /* kernel code, L=1 */
    gdt_set(gdt, 2, 0x00CF92000000FFFFull); /* kernel data */
    gdt_set(gdt, 3, 0x00CFF2000000FFFFull); /* user data, DPL3 */
    gdt_set(gdt, 4, 0x00AFFA000000FFFFull); /* user code, DPL3, L=1 */
    gdt[5] = (limit & 0xFFFFu) |
             ((base & 0xFFFFFFu) << 16) |
             ((uint64_t)0x89 << 40) |
             (((limit >> 16) & 0xFu) << 48) |
             (((base >> 24) & 0xFFu) << 56);
    gdt[6] = base >> 32;
    tss->iomap_base = sizeof(*tss);
    gdtr64_t gdtr = { sizeof(g_gdt[cpu]) - 1,
                      (uint64_t)(uintptr_t)g_gdt[cpu] };
    um_load_gdt(&gdtr);
    faults_set_kernel_cs(0x08);
    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" : : : "rax");
}

/* Called by hal_ctx_switch before entering a flow.  It keeps this CPU's
 * TSS.RSP0 on the kernel stack of whichever user process is about to run. */
void hal_user_bind_stack(void *top)
{
    uint32_t cpu = hal_cpu_self()->id;
    if (cpu >= FINTE_MAX_CPUS) {
        return;
    }
    if (!g_gdt_ready[cpu]) {
        build_gdt(cpu);
        g_gdt_ready[cpu] = 1;
    }
    uint64_t rsp0 = (uint64_t)(uintptr_t)top;
    g_tss[cpu].rsp0 = (rsp0 & ~0xFull) - 16u;
}

void usermode_prepare_cpu(uint32_t cpu)
{
    if (cpu >= FINTE_MAX_CPUS || g_gdt_ready[cpu]) {
        return;
    }
    build_gdt(cpu);
    g_gdt_ready[cpu] = 1;
}

void usermode_prepare(void)
{
    usermode_prepare_cpu(hal_cpu_self()->id);
}

void usermode_syscall(uint64_t *regs)
{
    fflow_t *f = finte_flow_current();
    fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
    uint64_t nr = regs[14];
    uint64_t a0 = regs[9];
    uint64_t a1 = regs[10];
    uint64_t a2 = regs[11];
    regs[14] = finte_proc_syscall(p, nr, a0, a1, a2, regs);
    if (p) {
        finte_process_signal_check(p, regs);
    }
}
