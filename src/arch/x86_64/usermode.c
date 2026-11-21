/* Ring-3 GDT/TSS and the int 0x80 syscall gate.
 *
 * Address spaces and ELF loading live in process.c.  This file only owns the
 * privileged transition state: the user GDT descriptors, the TSS (whose RSP0
 * follows the running process's kernel stack) and the syscall entry. */
#include "finte/finte.h"
#include "process.h"

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

static uint64_t g_gdt[7] __attribute__((aligned(16)));
static tss64_t g_tss __attribute__((aligned(16)));

extern void um_load_gdt(const gdtr64_t *g);
extern void um_enter(uint64_t rip, uint64_t rsp, uint64_t flags)
    __attribute__((noreturn));
extern void faults_set_kernel_cs(uint16_t cs);

static void gdt_set(int index, uint64_t value)
{
    g_gdt[index] = value;
}

static void build_gdt(void)
{
    uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    uint64_t limit = sizeof(g_tss) - 1;
    gdt_set(0, 0);
    gdt_set(1, 0x00AF9A000000FFFFull); /* kernel code, L=1 */
    gdt_set(2, 0x00CF92000000FFFFull); /* kernel data */
    gdt_set(3, 0x00CFF2000000FFFFull); /* user data, DPL3 */
    gdt_set(4, 0x00AFFA000000FFFFull); /* user code, DPL3, L=1 */
    g_gdt[5] = (limit & 0xFFFFu) |
               ((base & 0xFFFFFFu) << 16) |
               ((uint64_t)0x89 << 40) |
               (((limit >> 16) & 0xFu) << 48) |
               (((base >> 24) & 0xFFu) << 56);
    g_gdt[6] = base >> 32;
    g_tss.iomap_base = sizeof(g_tss);
    gdtr64_t gdtr = { sizeof(g_gdt) - 1, (uint64_t)(uintptr_t)g_gdt };
    um_load_gdt(&gdtr);
    faults_set_kernel_cs(0x08);
    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" : : : "rax");
}

/* Called by hal_ctx_switch before entering a flow.  It keeps TSS.RSP0 on the
 * kernel stack of whichever user process is about to run. */
void hal_user_bind_stack(void *top)
{
    /* User mode currently runs only on the BSP, which owns the single TSS.
     * AP context switches must not rewrite its RSP0. */
    if (hal_cpu_self()->id != 0) {
        return;
    }
    uint64_t rsp0 = (uint64_t)(uintptr_t)top;
    g_tss.rsp0 = (rsp0 & ~0xFull) - 16u;
}

void usermode_prepare(void)
{
    build_gdt();
}

uint64_t usermode_syscall(uint64_t *regs)
{
    return finte_process_syscall(regs);
}
