#include "finte/finte.h"

typedef struct __attribute__((packed)) idt_entry {
    uint16_t off_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} idt_entry_t;

typedef struct __attribute__((packed)) idt_ptr {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

extern void (*isr_table[32])(void);
extern void isr128(void);
extern void timer_isr(void);
extern void resched_isr(void);

static idt_entry_t g_idt[256];

uint64_t faults_idt_base(void)
{
    return (uint64_t)(uintptr_t)g_idt;
}

uint32_t faults_idt_limit(void)
{
    return (uint32_t)(sizeof(g_idt) - 1);
}

static void set_gate_sel(uint32_t vector, void (*fn)(void), uint8_t attr,
                         uint16_t selector)
{
    uint64_t addr = (uint64_t)(uintptr_t)fn;
    g_idt[vector].off_low = (uint16_t)(addr & 0xFFFFu);
    g_idt[vector].selector = selector;
    g_idt[vector].ist = 0;
    g_idt[vector].type_attr = attr;
    g_idt[vector].off_mid = (uint16_t)((addr >> 16) & 0xFFFFu);
    g_idt[vector].off_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    g_idt[vector].zero = 0;
}

static void set_gate(uint32_t vector, void (*fn)(void), uint8_t attr)
{
    uint16_t cs = 0;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    set_gate_sel(vector, fn, attr, cs);
}

void faults_init(void)
{
    finte_memset(g_idt, 0, sizeof(g_idt));
    for (uint32_t i = 0; i < 32; i++) {
        set_gate(i, isr_table[i], 0x8E);
    }
    /* User mode installs its own GDT with kernel code at 0x08. */
    set_gate_sel(0x80, isr128, 0xEE, 0x08); /* DPL 3 interrupt gate */
    set_gate(0x20, timer_isr, 0x8E);
    set_gate(0x21, resched_isr, 0x8E);
    idt_ptr_t ptr;
    ptr.limit = sizeof(g_idt) - 1;
    ptr.base = (uint64_t)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

/* The user-mode GDT puts kernel code at 0x08, so repoint the exception and
 * timer gates after loading it. The syscall gate is already 0x08. */
void faults_set_kernel_cs(uint16_t cs)
{
    for (uint32_t i = 0; i < 32; i++) {
        g_idt[i].selector = cs;
    }
    g_idt[0x20].selector = cs;
    g_idt[0x21].selector = cs;
}

void kx_fault_ex(uint64_t vec, uint64_t cs, uint64_t rip)
{
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if ((cs & 3u) == 3u) {
        finte_user_fault(vec, rip, cr2);
        /* No process owns this ring-3 context: fall through to the panic. */
    }
    char buf[224];
    int n = finte_format(buf, sizeof(buf),
                         "\n[kernel] EXCEPTION %llu cs=0x%llx rip=0x%llx "
                         "cr2=0x%llx -- halted\n",
                         (unsigned long long)vec,
                         (unsigned long long)cs,
                         (unsigned long long)rip,
                         (unsigned long long)cr2);
    hal_console_write(buf, (size_t)n);
    hal_exit(0x66);
}
