/* Blocks in ipc_recv so the kernel can exercise abort/teardown of a live
 * address space. */
#include "syscall.h"

#define SYS_IPC_RECV 211
#define SYS_EXIT 60

void _start(void)
{
    char buf[8];
    (void)fuser_syscall(SYS_IPC_RECV, 0, (long)buf, 0);
    fuser_syscall(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}
