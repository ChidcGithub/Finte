/* PSCI secondary-CPU entry. The context id is the logical CPU index. */
.section .text.boot, "ax"
.globl arm64_ap_entry
.type arm64_ap_entry, %function
arm64_ap_entry:
    mov x19, x0

    ldr x1, =vector_table
    msr vbar_el1, x1
    isb

    adrp x1, arm64_ap_stacks
    add x1, x1, :lo12:arm64_ap_stacks
    mov x2, #16384
    madd x1, x19, x2, x1
    add sp, x1, x2

    adrp x1, arm64_mmu_ttbr0
    add x1, x1, :lo12:arm64_mmu_ttbr0
    ldr x1, [x1]
    msr ttbr0_el1, x1
    adrp x1, arm64_mmu_tcr
    add x1, x1, :lo12:arm64_mmu_tcr
    ldr x1, [x1]
    msr tcr_el1, x1
    adrp x1, arm64_mmu_mair
    add x1, x1, :lo12:arm64_mmu_mair
    ldr x1, [x1]
    msr mair_el1, x1
    msr ttbr1_el1, xzr
    dsb sy
    isb
    adrp x1, arm64_mmu_sctlr
    add x1, x1, :lo12:arm64_mmu_sctlr
    ldr x1, [x1]
    msr sctlr_el1, x1
    isb
    ic iallu
    dsb nsh
    isb

    mov x0, x19
    bl arm64_ap_main
1:
    wfi
    b 1b
.size arm64_ap_entry, . - arm64_ap_entry
