#ifndef FINTE_PROCESS_H
#define FINTE_PROCESS_H

#include <stdint.h>

typedef struct fflow fflow_t;

/* User virtual layout.  The low 4 GiB stay supervisor-only in every process
 * page table; private user pages live at 4 GiB and above. */
#define FPROC_USER_BASE  0x100000000ull
#define FPROC_STACK_TOP  0x104000000ull
#define FPROC_SHARED_VA  0x108000000ull
#define FPROC_ARENA_VA   0x10C000000ull
#define FPROC_ARENA_MAX_PAGES 4
#define FPROC_USER_LIMIT 0x140000000ull

/* Native user syscalls (int 0x80). */
#define FPROC_SYS_WRITE     1
#define FPROC_SYS_GETPID    39
#define FPROC_SYS_EXIT      60
#define FPROC_SYS_TIME      200
#define FPROC_SYS_SLEEP     201
#define FPROC_SYS_YIELD     202
#define FPROC_SYS_IPC_SEND  210
#define FPROC_SYS_IPC_RECV  211
#define FPROC_SYS_SHM_ADDR  220
#define FPROC_SYS_ARENA_MAP 230

uint64_t finte_process_syscall(uint64_t *regs);
int finte_user_copy_in(fflow_t *f, void *dst, uint64_t uva, size_t len);
int finte_user_copy_out(fflow_t *f, uint64_t uva, const void *src, size_t len);

#endif
