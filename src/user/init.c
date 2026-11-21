/* Minimal ring-3 program. Loaded as an ELF64 image by the kernel. */
static const char message[] = "Hello from ring 3 via int 0x80\n";

static long syscall3(long nr, long a, long b)
{
    register long rax __asm__("rax") = nr;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi)
                     : "memory");
    return rax;
}

void _start(void)
{
    syscall3(1, (long)message, (long)sizeof(message) - 1);
    syscall3(60, 0, 0);
    for (;;) {
    }
}
