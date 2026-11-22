/* Shared user-process layer: address spaces, ELF loading, syscalls and ABI.
 *
 * Architecture backends provide page-table construction, root switching,
 * EL0/ring-3 entry, trap-frame copy and NX/read-only PTE encoding. Everything
 * else lives here so x86_64 and arm64 share one user ABI. */
#ifndef FINTE_PROCESS_H
#define FINTE_PROCESS_H

#include "finte/base.h"
#include "finte/object.h"

/* User virtual layout. 4-5 GiB is the private process window; the kernel's
 * low identity map stays supervisor-only in every process root. */
#define FPROC_USER_BASE        0x100000000ull
#define FPROC_STACK_TOP        0x104000000ull
#define FPROC_SHARED_VA        0x108000000ull
#define FPROC_ARENA_VA         0x10C000000ull
#define FPROC_MMAP_BASE        0x110000000ull
#define FPROC_MMAP_LIMIT       0x13C000000ull
#define FPROC_USER_LIMIT       0x140000000ull
#define FPROC_ARENA_MAX_PAGES  4

#define FPROC_MAX_PAGES        512
#define FPROC_MAX_FDS          256
#define FPROC_MAX_OFDS         256
#define FPROC_MAX_MAPS         16
#define FPROC_MAX_MAP_PAGES    64
#define FPROC_MAX_PROCS        16
#define FPROC_MAX_SIGNALS      32
#define FPROC_STACK_PAGES      4
#define FPROC_STACK_BYTES      (FPROC_STACK_PAGES * 4096u)

#define FPROC_PROT_READ        0x1u
#define FPROC_PROT_WRITE       0x2u
#define FPROC_PROT_EXEC        0x4u

#define FPROC_OPEN_READ        0x1u
#define FPROC_OPEN_WRITE       0x2u
#define FPROC_OPEN_CREATE      0x4u
#define FPROC_OPEN_TRUNC       0x8u

#define FPROC_FD_NONE          0u
#define FPROC_FD_CONSOLE       1u
#define FPROC_FD_INITRD        2u
#define FPROC_FD_FILE          3u

/* Native syscalls. The existing demo numbers stay stable. */
#define FPROC_SYS_WRITE        1
#define FPROC_SYS_READ         3
#define FPROC_SYS_OPEN         4
#define FPROC_SYS_CLOSE        5
#define FPROC_SYS_LSEEK        6
#define FPROC_SYS_MMAP         7
#define FPROC_SYS_MUNMAP       8
#define FPROC_SYS_FORK         9
#define FPROC_SYS_EXEC         10
#define FPROC_SYS_KILL         11
#define FPROC_SYS_SIGNAL       12
#define FPROC_SYS_SIGRETURN    13
#define FPROC_SYS_WRITE_FD     14
#define FPROC_SYS_SERVICE_REG  15
#define FPROC_SYS_SERVICE_LOOK 16
#define FPROC_SYS_SERVICE_UNREG 17
#define FPROC_SYS_DYNSVC       18
#define FPROC_SYS_MMAP_FILE    19
#define FPROC_SYS_DUP          32
#define FPROC_SYS_DUP2         33
#define FPROC_SYS_GETITIMER    36
#define FPROC_SYS_ALARM        37
#define FPROC_SYS_SETITIMER    38
#define FPROC_SYS_GETPID       39
#define FPROC_SYS_WAITPID      40
#define FPROC_WNOHANG          1
#define FPROC_WUNTRACED        2
#define FPROC_WCONTINUED       4
#define FPROC_SYS_EXIT         60
#define FPROC_SYS_FCNTL        72
#define FPROC_F_DUPFD          0
#define FPROC_F_GETFD          1
#define FPROC_F_SETFD          2
#define FPROC_F_GETFL          3
#define FPROC_F_SETFL          4
#define FPROC_FD_CLOEXEC       0x1
#define FPROC_SYS_TIME         200
#define FPROC_SYS_SLEEP        201
#define FPROC_SYS_YIELD        202
#define FPROC_SYS_IPC_SEND     210
#define FPROC_SYS_IPC_RECV     211
#define FPROC_SYS_IPC_SEND_CAP 212
#define FPROC_SYS_IPC_RECV_CAP 213
#define FPROC_SYS_SHM_ADDR     220
#define FPROC_SYS_ARENA_MAP    230

#define FPROC_SIGINT           2
#define FPROC_SIGKILL          9
#define FPROC_SIGUSR1          10
#define FPROC_SIGSEGV          11
#define FPROC_SIGUSR2          12
#define FPROC_SIGALRM          14
#define FPROC_SIGTERM          15
#define FPROC_SIGCHLD          17
#define FPROC_SIGCONT          18
#define FPROC_SIGSTOP          19
#define FPROC_SIGVTALRM        26
#define FPROC_SIGPROF          27
#define FPROC_SYS_SIGPROCMASK  41
#define FPROC_SYS_SETPGID      42
#define FPROC_SYS_GETPGRP      43
#define FPROC_SIG_BLOCK        0
#define FPROC_SIG_UNBLOCK      1
#define FPROC_SIG_SETMASK      2

typedef struct faddr_space faddr_space_t;
typedef struct fproc fproc_t;
typedef struct fproc_arch_ops fproc_arch_ops_t;

typedef struct ffile_desc ffile_desc_t;

/* A descriptor is a reference to a shared open-file description. Dups and
 * fork children share the description and therefore the offset. */
typedef struct ffd {
    ffile_desc_t *ofd;
    uint8_t fd_flags;
} ffd_t;

typedef struct fmap {
    uint8_t used;
    uint8_t prot;
    uint16_t page_count;
    uint64_t va;
    uint64_t len;
    uint64_t pa[FPROC_MAX_MAP_PAGES];
} fmap_t;

typedef struct fsig {
    uint64_t handler;
    uint64_t restorer;
    uint32_t pending;
} fsig_t;


struct faddr_space {
    uint32_t refs;
    uint32_t asid;
    uint64_t root;
    uint64_t kernel_root;
    uint64_t pages[FPROC_MAX_PAGES];
    uint32_t page_count;
    const fproc_arch_ops_t *ops;
    void *arch;
};

struct fproc {
    uint32_t pid;
    uint32_t pgid;
    uint32_t cpu;
    faddr_space_t *space; /* weak: the Flow owns the reference */
    uint64_t entry;
    uint64_t stack_top;
    uint64_t shared_phys;
    fflow_t *flow;
    fhandle_t flow_handle;
    int ipc_cap;
    int arena_cap;
    int faulted;
    int killed;
    uint32_t exit_code;
    uint8_t expected_fault;
    uint8_t fork_child;
    uint8_t stopped;
    uint8_t stop_pending;
    uint8_t stop_acked;
    uint8_t cont_pending;
    uint64_t timer_expire_ns;   /* absolute; 0 = disarmed */
    uint64_t timer_interval_ns; /* 0 = one-shot */
    uint64_t vtimer_left_ns;    /* ITIMER_VIRTUAL remaining CPU time */
    uint64_t vtimer_interval_ns;
    uint64_t ptimer_left_ns;    /* ITIMER_PROF remaining CPU time */
    uint64_t ptimer_interval_ns;
    char name[FINTE_NAME_MAX];
    ffd_t *fds;
    uint32_t fd_capacity;
    fmap_t maps[FPROC_MAX_MAPS];
    uint64_t mmap_next;
    fsig_t sigs[FPROC_MAX_SIGNALS];
    uint32_t sig_blocked;
    void *sig_frame;
    uint64_t sig_ret;
    void *fork_frame;
    struct fproc *parent;
    struct fproc *children;
    struct fproc *next_sibling;
};

struct fproc_arch_ops {
    uint16_t elf_machine;
    const char *name;
    int (*space_create)(faddr_space_t *as);
    void (*space_destroy)(faddr_space_t *as);
    int (*space_clone)(faddr_space_t *dst, faddr_space_t *src);
    int (*map_page)(faddr_space_t *as, uint64_t va, uint64_t pa,
                    uint32_t prot);
    int (*unmap_page)(faddr_space_t *as, uint64_t va, uint64_t *pa);
    uint64_t (*translate)(faddr_space_t *as, uint64_t va, int write);
    uint32_t (*prot)(faddr_space_t *as, uint64_t va);
    uint64_t (*root)(faddr_space_t *as);
    void (*switch_root)(faddr_space_t *as);
    uint64_t (*kernel_root)(void);
    void (*flush_space)(faddr_space_t *as);
    void (*enter_user)(uint64_t entry, uint64_t usp,
                       uint64_t kstack_top) __attribute__((noreturn));
    void (*enter_frame)(void *frame,
                        uint64_t kstack_top) __attribute__((noreturn));
    void (*exec_frame)(void *frame, uint64_t entry, uint64_t sp);
    void (*prepare)(void);
    uint32_t (*frame_size)(void);
    void (*fork_frame)(void *dst, const void *src);
    int (*signal_setup)(faddr_space_t *as, void *frame, uint64_t handler,
                        uint64_t restorer, uint32_t sig);
    uint64_t (*signal_restore)(void *frame, const void *saved);
};

/* fproc_arch() is implemented by the selected architecture. */
const fproc_arch_ops_t *fproc_arch(void);

faddr_space_t *finte_as_create(void);
faddr_space_t *finte_as_clone(faddr_space_t *src);
faddr_space_t *finte_as_ref(faddr_space_t *as);
void finte_as_unref(faddr_space_t *as);
void *finte_as_alloc_page(faddr_space_t *as);
void finte_as_free_page(faddr_space_t *as, void *page);
int finte_as_map(faddr_space_t *as, uint64_t va, uint64_t pa, uint32_t prot);
int finte_as_unmap(faddr_space_t *as, uint64_t va, uint64_t *pa);
uint64_t finte_as_translate(faddr_space_t *as, uint64_t va, int write);
uint32_t finte_as_prot(faddr_space_t *as, uint64_t va);
int finte_as_user_ok(faddr_space_t *as, uint64_t va, uint64_t len, int write);
uint64_t finte_as_root(faddr_space_t *as);
void finte_as_switch(faddr_space_t *as);
void finte_as_switch_kernel(void);
void finte_as_flush(faddr_space_t *as);
uint32_t finte_as_live_spaces(void);
int finte_as_has_page(faddr_space_t *as, uint64_t pa);
int finte_as_share_page(faddr_space_t *as, uint64_t pa);
int finte_as_forget_page(faddr_space_t *as, uint64_t pa);
uint32_t finte_cow_prot(uint64_t pa);
int finte_cow_share(uint64_t pa, uint32_t prot);
int finte_cow_put(uint64_t pa);
int finte_as_cow_fault(faddr_space_t *as, uint64_t va, int write);

fproc_t *finte_proc_create(const char *name, const char *elf, uint32_t pid,
                           uint64_t shared_phys, fhandle_t chan,
                           uint32_t chan_rights, fhandle_t arena,
                           uint32_t arena_page, uint32_t arena_rights);
int finte_proc_start(fproc_t *p);
void finte_proc_destroy(fproc_t *p);
fproc_t *finte_proc_find(uint32_t pid);
int finte_proc_waitpid(fproc_t *p, int32_t pid, uint64_t status_uva,
                       int options);
uint64_t finte_proc_syscall(fproc_t *p, uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2, void *frame);
int finte_user_copy_in(fflow_t *f, void *dst, uint64_t uva, size_t len);
int finte_user_copy_out(fflow_t *f, uint64_t uva, const void *src, size_t len);
void finte_user_fault(uint64_t vec, uint64_t rip, uint64_t cr2);
void finte_process_signal_check(fproc_t *p, void *frame);
int finte_proc_signal_pending(const fproc_t *p);
void finte_proc_timer_tick(void);
void finte_proc_cpu_tick(uint64_t ns, int user);
int finte_usermode_run(const char *name);
int finte_proc_demo(void);
int finte_proc_abi_test(void);
int finte_proc_churn(uint32_t count);
void finte_proc_abort(fproc_t *p);

#endif /* FINTE_PROCESS_H */
