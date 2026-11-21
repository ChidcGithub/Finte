/* The Finte verb ABI.
 *
 * A verb is a small, fixed request. Arguments are values, never paths. Bulk
 * data is represented by a capability, which is why a message is 48 bytes.
 */
#ifndef FINTE_VERB_H
#define FINTE_VERB_H

#include "finte/object.h"

enum {
    FINTE_VERB_NOP = 0,
    FINTE_VERB_SYS_INFO = 1,
    FINTE_VERB_CAP_INFO = 2,
    FINTE_VERB_CAP_DUP = 3,
    FINTE_VERB_CAP_DROP = 4,
    FINTE_VERB_CAP_GRANT = 5,
    FINTE_VERB_CHAN_SEND = 6,
    FINTE_VERB_CHAN_RECV = 7,
    FINTE_VERB_CHAN_INFO = 8,
    FINTE_VERB_FLOW_YIELD = 9,
    FINTE_VERB_FLOW_EXIT = 10,
    FINTE_VERB_FLOW_INFO = 11,
    FINTE_VERB_FLOW_FUEL = 12,
    FINTE_VERB_ARENA_ALLOC = 13,
    FINTE_VERB_ARENA_FREE = 14,
    FINTE_VERB_CONSOLE_WRITE = 15,
    FINTE_VERB_FILE_READ = 16,
    FINTE_VERB_CLOCK_NOW = 17,
    FINTE_VERB_SYS_STATS = 18,
    FINTE_VERB_SYS_DUMP = 19,
    FINTE_VERB_FLOW_WAIT = 20,
    FINTE_VERB_CHAN_CLOSE = 21,
    FINTE_VERB_FILE_WRITE = 22,
    FINTE_VERB__COUNT = 23
};

typedef struct fcap_info {
    uint32_t type;
    uint32_t rights;
    uint64_t handle;
    uint64_t aux;
    uint64_t offset;
    uint64_t length;
    uint32_t flags;
    uint32_t pad;
} fcap_info_t;

#define FINTE_OPEN_READ 1u
#define FINTE_OPEN_WRITE 2u
#define FINTE_OPEN_CREATE 4u
#define FINTE_OPEN_TRUNC 8u

int finte_disk_file_open(const char *path, uint32_t flags, fhandle_t *out);
int64_t finte_file_handle_read(fhandle_t h, void *buf, size_t len);
int64_t finte_file_handle_write(fhandle_t h, const void *buf, size_t len);
int finte_file_handle_seek(fhandle_t h, uint64_t off);
int64_t finte_file_handle_size(fhandle_t h);
int finte_file_handle_close(fhandle_t h);

typedef struct fflow_info {
    uint64_t handle;
    uint32_t state;
    uint32_t persona;
    uint32_t fuel;
    uint32_t fuel_max;
    uint32_t caps;
    uint32_t pad;
    char name[FINTE_NAME_MAX];
} fflow_info_t;

typedef struct fchan_info {
    uint64_t handle;
    uint32_t capacity;
    uint32_t count;
    uint32_t closed;
    uint32_t pad;
    uint64_t sent;
    uint64_t received;
    uint64_t waiting_send;
    uint64_t waiting_recv;
    char name[FINTE_NAME_MAX];
} fchan_info_t;

fret_t finte_syscall(fflow_t *f, uint32_t nr, const uint64_t args[6]);

fret_t fx_send(fflow_t *f, uint16_t cap, uint32_t tag,
               const void *data, uint16_t len);
fret_t fx_try_send(fflow_t *f, uint16_t cap, uint32_t tag,
                   const void *data, uint16_t len);
fret_t fx_send_msg(fflow_t *f, uint16_t cap, fmsg_t *msg, uint32_t flags);
fret_t fx_recv(fflow_t *f, uint16_t cap, fmsg_t *out);
fret_t fx_try_recv(fflow_t *f, uint16_t cap, fmsg_t *out);
fret_t fx_recv_msg(fflow_t *f, uint16_t cap, fmsg_t *out, uint32_t flags);
fret_t fx_console_write(fflow_t *f, uint16_t cap, const void *buf, size_t len);
fret_t fx_cap_info(fflow_t *f, uint16_t cap, fcap_info_t *out);
fret_t fx_cap_dup(fflow_t *f, uint16_t cap, uint32_t rights);
fret_t fx_cap_drop(fflow_t *f, uint16_t cap);
fret_t fx_cap_grant(fflow_t *f, uint16_t flow_cap, uint16_t src_cap,
                    uint32_t rights);
fret_t fx_flow_info(fflow_t *f, fflow_info_t *out);
fret_t fx_flow_fuel(fflow_t *f, uint32_t *out);
fret_t fx_flow_wait(fflow_t *f, uint16_t flow_cap);
fret_t fx_chan_info(fflow_t *f, uint16_t cap, fchan_info_t *out);
fret_t fx_chan_close(fflow_t *f, uint16_t cap);
fret_t fx_arena_alloc(fflow_t *f, uint16_t cap, size_t size, size_t align,
                      uint64_t *offset);
fret_t fx_arena_free(fflow_t *f, uint16_t cap, uint64_t offset);
fret_t fx_file_read(fflow_t *f, uint16_t cap, uint64_t offset,
                    void *buf, size_t len);
fret_t fx_file_write(fflow_t *f, uint16_t cap, uint64_t offset,
                     const void *buf, size_t len);
fret_t fx_clock_now(fflow_t *f, uint64_t *ns);
fret_t fx_sys_stats(fflow_t *f, fstats_t *out);
void fx_yield(fflow_t *f);
void fx_sleep_ns(fflow_t *f, uint64_t ns);
void fx_print(fflow_t *f, uint16_t console_cap, const char *fmt, ...);
void fx_println(fflow_t *f, uint16_t console_cap, const char *fmt, ...);

#endif /* FINTE_VERB_H */
