/* Nodes, capabilities, flows, channels, arenas and the core registry. */
#ifndef FINTE_OBJECT_H
#define FINTE_OBJECT_H

#include "finte/base.h"
#include "finte/hal.h"
#include "finte/disk.h"

typedef struct fflow fflow_t;
typedef struct fthread fthread_t;
struct faddr_space;
typedef struct fchan fchan_t;
typedef struct farena farena_t;
typedef struct ffile ffile_t;
typedef struct fdir fdir_t;

typedef struct fnode {
    uint16_t type;
    uint16_t flags;
    uint32_t gen;
    uint32_t refs;
    uint32_t index;
    uint32_t aux;
} fnode_t;

typedef struct fcap {
    fhandle_t handle;
    uint32_t rights;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
} fcap_t;

#define FINTE_CAP_SCOPED 0x01u

typedef enum {
    FINTE_FLOW_FREE = 0,
    FINTE_FLOW_READY = 1,
    FINTE_FLOW_RUNNING = 2,
    FINTE_FLOW_BLOCKED = 3,
    FINTE_FLOW_SLEEPING = 4,
    FINTE_FLOW_THROTTLED = 5,
    FINTE_FLOW_DEAD = 6
} fflow_state_t;

/* Thread states intentionally share the numeric values of the matching Flow
 * states. A Flow's state mirrors its primary Thread in the serial-only stage. */
typedef enum {
    FINTE_THREAD_FREE = FINTE_FLOW_FREE,
    FINTE_THREAD_READY = FINTE_FLOW_READY,
    FINTE_THREAD_RUNNING = FINTE_FLOW_RUNNING,
    FINTE_THREAD_BLOCKED = FINTE_FLOW_BLOCKED,
    FINTE_THREAD_SLEEPING = FINTE_FLOW_SLEEPING,
    FINTE_THREAD_THROTTLED = FINTE_FLOW_THROTTLED,
    FINTE_THREAD_DEAD = FINTE_FLOW_DEAD,
    FINTE_THREAD_CREATED = 7
} fthread_state_t;

typedef enum {
    FINTE_PERSONA_NATIVE = 0,
    FINTE_PERSONA_LINUX = 1
} fpersona_t;

typedef struct fprogram {
    const char *name;
    void (*entry)(fflow_t *flow);
    uint32_t default_fuel;
} fprogram_t;

typedef struct flinux_state {
    int32_t fd_cap[FINTE_MAX_CAPS];
    uint64_t fd_pos[FINTE_MAX_CAPS];
    uint64_t brk;
    uint64_t brk_end;
    uint64_t heap_base;
    int32_t console_cap;
    int32_t dir_cap;
    int32_t arena_cap;
    int32_t last_errno;
} flinux_state_t;

/* A Thread is the CPU execution entity. In the serial-only stage each Flow
 * owns exactly one Thread. The Flow keeps task state and the Thread keeps the
 * context, stack, CPU affinity and scheduler links. */
struct fthread {
    uint8_t in_use;
    uint8_t state;
    uint8_t queued;
    uint8_t blocking;
    uint8_t wake_pending;
    uint8_t wait_send;
    uint16_t slot;
    uint32_t cpu;
    uint32_t priority;
    uint32_t quantum;
    uint64_t cycles;
    uint64_t wake_ns;
    fflow_t *flow;
    fchan_t *wait_chan;
    struct fthread *next_run;
    struct fthread *next_throttle;
    struct fthread *next_sleep;
    struct fthread *wait_next;
    struct faddr_space *release_space;
    fhal_ctx_t *ctx;
};

struct fflow {
    uint8_t in_use;
    uint8_t state;
    uint8_t persona;
    uint8_t started;
    uint8_t abort;
    uint8_t reserved0;
    uint16_t slot;
    uint16_t cap_count;
    uint32_t fuel;
    uint32_t fuel_max;
    uint32_t exit_code;
    fhandle_t self;
    const fprogram_t *prog;
    fcap_t caps[FINTE_MAX_CAPS];
    fthread_t *thread;
    struct fthread *waiters;
    fflow_stats_t stats;
    flinux_state_t linux;
    void *user;
    struct faddr_space *space;
    char name[FINTE_NAME_MAX];
};

struct fchan {
    uint8_t in_use;
    uint8_t closed;
    uint16_t capacity;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint32_t aux;
    fhandle_t self;
    fthread_t *wait_send;
    fthread_t *wait_recv;
    uint64_t sent;
    uint64_t received;
    fmsg_t ring[FINTE_CHAN_CAP];
    fcap_t qcaps[FINTE_CHAN_CAP][FINTE_MSG_CAPS];
    uint8_t qcap_count[FINTE_CHAN_CAP];
    char name[FINTE_NAME_MAX];
};

typedef struct fblock {
    size_t size;
    struct fblock *next;
    uint32_t free;
    uint32_t magic;
    uint32_t scoped;
    uint32_t pad;
} fblock_t;

#define FINTE_ARENA_BLOCK_HDR ((sizeof(fblock_t) + 15u) & ~(size_t)15u)

struct farena {
    uint8_t in_use;
    uint8_t pad[7];
    uint8_t *base;
    size_t size;
    size_t used;
    fblock_t *free_list;
    fhandle_t self;
    uint64_t allocs;
    uint64_t frees;
    uint64_t live_bytes;
    uint64_t peak_bytes;
    char name[FINTE_NAME_MAX];
};

struct ffile {
    uint8_t in_use;
    uint8_t writable;
    uint8_t backend;      /* 0 = static ramdisk data, 1 = mounted file */
    uint8_t pad;
    uint32_t rights;
    const uint8_t *data;
    size_t size;
    uint64_t offset;
    char path[FDISK_PATH_MAX];
    char name[FINTE_NAME_MAX];
};

struct fdir {
    uint8_t in_use;
    uint8_t pad[3];
    uint16_t count;
    fhandle_t self;
    char name[FINTE_NAME_MAX];
    struct {
        char name[FINTE_NAME_MAX];
        fhandle_t file;
    } ents[FINTE_DIR_ENTS];
};

typedef struct finte_core {
    int initialized;
    int status;
    uint32_t epochs;
    uint32_t rng;
    fnode_t nodes[FINTE_MAX_NODES];
    fflow_t flows[FINTE_MAX_FLOWS];
    fthread_t threads[FINTE_MAX_THREADS];
    fchan_t channels[FINTE_MAX_CHANNELS];
    farena_t arenas[FINTE_MAX_ARENAS];
    ffile_t files[FINTE_MAX_FILES];
    fdir_t dirs[FINTE_MAX_DIRS];
    fstats_t stats;
    fthread_t *throttle;
    fthread_t *sleepers;
    fhandle_t console_default;
    uint64_t boot_cycles;
} finte_core_t;

extern finte_core_t finte_core;

/* Lifecycle. */
void finte_init(void);
void finte_reset(void);
int finte_is_initialized(void);

/* Node factories. Each returns a handle; caps are how authority is obtained. */
fhandle_t finte_console_create(const char *name);
fhandle_t finte_channel_create(const char *name, uint16_t capacity);
fhandle_t finte_arena_create(const char *name, size_t bytes);
fhandle_t finte_file_create(const char *name, const void *data, size_t size);
fhandle_t finte_dir_create(const char *name);
fret_t finte_dir_add(fhandle_t dir, const char *name, fhandle_t file);

/* Flows are linked images with explicit caps; there is no fork and no exec. */
fhandle_t finte_flow_create(const char *name, const fprogram_t *prog,
                            uint32_t fuel, uint8_t persona);
fret_t finte_flow_start(fhandle_t flow);
fhandle_t finte_flow_handle(fflow_t *flow);
fflow_t *finte_flow_current(void);
fflow_t *finte_flow_lookup(fhandle_t flow);
int finte_flow_alive(fhandle_t flow);

/* Scheduler. Runs until every flow is dead, or reports a deadlock. */
void finte_run(void);
int finte_run_status(void);
int finte_sched_active(void);
void finte_sched_ap_run(void) __attribute__((noreturn));
void finte_sched_reset(void);
int finte_sched_cpu_has_work(void);
fstats_t finte_get_stats(void);
void finte_dump(const char *title);

/* Global core lock. Blocking verbs release it around the context switch. */
void finte_core_lock(void);
void finte_core_unlock(void);
uint32_t finte_core_lock_suspend(void);
void finte_core_lock_resume(uint32_t depth);

/* Node table. */
fnode_t *finte_node_get(fhandle_t h);
fhandle_t finte_node_alloc(uint16_t type, uint32_t index, uint32_t aux);
void finte_node_ref(fhandle_t h);
void finte_node_unref(fhandle_t h);
uint32_t finte_node_type(fhandle_t h);
uint32_t finte_node_refs(fhandle_t h);

/* Capability table operations. */
int finte_cap_install(fflow_t *f, fhandle_t h, uint32_t rights);
int finte_cap_install_scoped(fflow_t *f, fhandle_t h, uint32_t rights,
                             uint64_t offset, uint64_t length);
int finte_cap_install_cap(fflow_t *f, const fcap_t *cap);
int finte_cap_install_arena_pages(fflow_t *f, fhandle_t arena,
                                  uint32_t first_page, uint32_t pages,
                                  uint32_t rights);
int finte_cap_drop_index(fflow_t *f, uint16_t cap);
int finte_cap_check(fflow_t *f, uint16_t cap, uint32_t need, fcap_t *out);
void finte_cap_drop_all(fflow_t *f);
int finte_cap_free_slots(fflow_t *f);

/* Channel internals (used by the verb dispatcher and tests). */
fret_t finte_chan_send(fflow_t *f, uint16_t cap, fmsg_t *msg, uint32_t flags);
fret_t finte_chan_recv(fflow_t *f, uint16_t cap, fmsg_t *out, uint32_t flags);
fret_t finte_chan_close(fflow_t *f, uint16_t cap);
void finte_chan_release_queue(fchan_t *ch);

/* Flow scheduling primitives. */
void finte_flow_yield(fflow_t *f);
void finte_flow_sleep(fflow_t *f, uint64_t ns);
void finte_flow_block(fflow_t *f, fchan_t *ch, int is_send);
void finte_flow_wait_for(fflow_t *f, fflow_t *target);
void finte_flow_stop(fflow_t *f);
void finte_flow_exit(fflow_t *f, int code) __attribute__((noreturn));
void finte_sched_wake(fflow_t *f);
void finte_sched_enqueue(fflow_t *f);
void finte_sched_switch_out(fflow_t *f);
void finte_sched_preempt(fflow_t *f);
void finte_sched_sleep(fflow_t *f, uint64_t wake_ns);
void finte_sched_wake_thread(fthread_t *t);
void finte_sched_enqueue_thread(fthread_t *t);
void finte_sched_switch_out_thread(fthread_t *t);
void finte_sched_preempt_thread(fthread_t *t);
void finte_sched_sleep_thread(fthread_t *t, uint64_t wake_ns);
void finte_sched_throttle_thread(fthread_t *t);
fthread_t *finte_thread_current(void);
void finte_thread_release(fthread_t *t);
void finte_sched_tick(void);
int finte_sched_migrate(fflow_t *f, uint32_t target);

/* Arena internals. */
fret_t finte_arena_alloc(farena_t *a, size_t size, size_t align, uint64_t *offset);
fret_t finte_arena_alloc_scoped(farena_t *a, uint64_t offset, uint64_t length,
                                size_t size, size_t align, uint64_t *out);
fret_t finte_arena_reserve(farena_t *a, uint64_t offset, uint64_t length);
fret_t finte_arena_free(farena_t *a, uint64_t offset);
fret_t finte_arena_free_scoped(farena_t *a, uint64_t offset, int scoped);
void finte_arena_destroy(farena_t *a);

#endif /* FINTE_OBJECT_H */
