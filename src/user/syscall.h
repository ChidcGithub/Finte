#ifndef FINTE_USER_SYSCALL_H
#define FINTE_USER_SYSCALL_H

/* Native user syscall ABI: x86_64 uses int 0x80 with rax/rdi/rsi/rdx;
 * AArch64 uses svc #0 with x8 = number and x0-x2 = arguments. */
static inline long fuser_syscall(long nr, long a, long b, long c)
{
#if defined(__aarch64__)
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
#else
    register long rax __asm__("rax") = nr;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "memory");
    return rax;
#endif
}

#endif /* FINTE_USER_SYSCALL_H */
