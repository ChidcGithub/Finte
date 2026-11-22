/* Dynamic shared-object smoke test: ask the kernel to load libsvc.so, then
 * call its exported function through the returned address. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_EXIT 60
#define SYS_DYNSVC 18

typedef int (*svc_add_fn)(int, int);

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
    static const char name[] = "libsvc.so";
    long addr = fuser_syscall(SYS_DYNSVC, (long)name, sizeof(name) - 1, 0);
    if (addr <= 0) {
        print("[abi] dyn load FAIL\n");
        fuser_syscall(SYS_EXIT, 1, 0, 0);
    }
    svc_add_fn fn = (svc_add_fn)(long)addr;
    int sum = fn(2, 3);
    print(sum == 5 ? "[abi] dyn 2+3=5\n" : "[abi] dyn FAIL\n");
    fuser_syscall(SYS_EXIT, sum == 5 ? 0 : 1, 0, 0);
    for (;;) {
    }
}
