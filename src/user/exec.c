/* exec() smoke test: replace this image with init.elf. */
#include "syscall.h"

#define SYS_EXEC 10
#define SYS_EXIT 60
#define SYS_WRITE 1

static void print(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    fuser_syscall(SYS_WRITE, (long)s, n, 0);
}

void _start(void)
{
    static const char image[] = "init.elf";
    long rc = fuser_syscall(SYS_EXEC, (long)image, sizeof(image) - 1, 0);
    (void)rc;
    print("[exec] FAIL\n");
    fuser_syscall(SYS_EXIT, 1, 0, 0);
    for (;;) {
    }
}
