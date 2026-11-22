/* Finte core primitives.
 *
 * Finte is built around three ideas:
 *   1. Every resource is a node with a generation-checked handle.
 *   2. Authority is explicit: a flow can only touch what it holds a cap for.
 *   3. Every flow has a finite fuel budget. Nothing runs forever by accident.
 */
#ifndef FINTE_BASE_H
#define FINTE_BASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "finte/arch.h"

#define FINTE_VERSION_MAJOR 0
#define FINTE_VERSION_MINOR 0
#define FINTE_VERSION_PATCH 2

/* The build generates build/finte_version.h with a short commit suffix.
 * Tools that compile the headers directly keep the plain fallback. */
#if defined(__has_include)
#if __has_include("finte_version.h")
#include "finte_version.h"
#endif
#endif
#ifndef FINTE_VERSION_STRING
#define FINTE_VERSION_STRING "0.0.2"
#endif
#define FINTE_NICKNAME "base"

#define FINTE_NAME_MAX 24
#define FINTE_MSG_DATA 32
#define FINTE_MSG_CAPS 2

#define FINTE_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#ifndef FINTE_MAX_NODES
#define FINTE_MAX_NODES 512
#endif
#ifndef FINTE_MAX_FLOWS
#define FINTE_MAX_FLOWS 64
#endif
#ifndef FINTE_MAX_THREADS
#define FINTE_MAX_THREADS 64
#endif
#ifndef FINTE_MAX_CHANNELS
#define FINTE_MAX_CHANNELS 64
#endif
#ifndef FINTE_MAX_ARENAS
#define FINTE_MAX_ARENAS 16
#endif
#ifndef FINTE_MAX_FILES
#define FINTE_MAX_FILES 64
#endif
#ifndef FINTE_MAX_DIRS
#define FINTE_MAX_DIRS 8
#endif
#ifndef FINTE_MAX_CAPS
#define FINTE_MAX_CAPS 32
#endif
#ifndef FINTE_CHAN_CAP
#define FINTE_CHAN_CAP 32
#endif
#ifndef FINTE_DIR_ENTS
#define FINTE_DIR_ENTS 24
#endif

/* Charged from a flow's fuel every time the scheduler runs it. */
#ifndef FINTE_FUEL_QUANTUM
#define FINTE_FUEL_QUANTUM 24
#endif

/* A handle is (generation << 32) | (slot + 1). Zero is never valid. */
typedef uint64_t fhandle_t;
#define FINTE_HANDLE_NONE ((fhandle_t)0)

static inline fhandle_t finte_handle_make(uint32_t slot, uint32_t gen)
{
    return ((uint64_t)gen << 32) | ((uint64_t)slot + 1u);
}
static inline uint32_t finte_handle_slot(fhandle_t h)
{
    return (uint32_t)(h & 0xFFFFFFFFu) - 1u;
}
static inline uint32_t finte_handle_gen(fhandle_t h)
{
    return (uint32_t)(h >> 32);
}
static inline int finte_handle_valid(fhandle_t h)
{
    return (h & 0xFFFFFFFFu) != 0u;
}

typedef enum {
    FINTE_OK = 0,
    FINTE_ERR_INVAL = -1,
    FINTE_ERR_NOENT = -2,
    FINTE_ERR_NOMEM = -3,
    FINTE_ERR_PERM = -4,
    FINTE_ERR_BUSY = -5,
    FINTE_ERR_AGAIN = -6,
    FINTE_ERR_LIMIT = -7,
    FINTE_ERR_IO = -8,
    FINTE_ERR_DEADLOCK = -9,
    FINTE_ERR_NOSYS = -10,
    FINTE_ERR_FAULT = -11,
    FINTE_ERR_RANGE = -12,
    FINTE_ERR_EXIST = -13,
    FINTE_ERR_TYPE = -14,
    FINTE_ERR_STATE = -15,
    FINTE_ERR_EOF = -16
} fstatus_t;

/* Finte returns status and value separately. Unlike errno-style APIs, a
 * successful call can never be confused with an error. */
typedef struct fret {
    int32_t status;
    uint32_t detail;
    uint64_t value;
} fret_t;

static inline fret_t finte_ret(int32_t status, uint64_t value)
{
    fret_t r;
    r.status = status;
    r.detail = 0;
    r.value = value;
    return r;
}
static inline fret_t finte_ret_ok(uint64_t value)
{
    return finte_ret(FINTE_OK, value);
}
static inline int finte_failed(fret_t r)
{
    return r.status != FINTE_OK;
}

const char *finte_strstatus(int32_t status);

/* Capability rights. Nodes can be shared with any subset of these. */
#define FINTE_R_READ    (1u << 0)
#define FINTE_R_WRITE   (1u << 1)
#define FINTE_R_SEND    (1u << 2)
#define FINTE_R_RECV    (1u << 3)
#define FINTE_R_EXEC    (1u << 4)
#define FINTE_R_GRANT   (1u << 5)
#define FINTE_R_CONTROL (1u << 6)
#define FINTE_R_SIGNAL  (1u << 7)
#define FINTE_R_ALL     0xFFu

/* Node types. The core has no path namespace: directories and names are a
 * persona-level policy, which is why the Linux personality can provide one
 * without the core knowing what a path is. */
typedef enum {
    FINTE_NODE_FREE = 0,
    FINTE_NODE_CONSOLE = 1,
    FINTE_NODE_CHANNEL = 2,
    FINTE_NODE_ARENA = 3,
    FINTE_NODE_FLOW = 4,
    FINTE_NODE_FILE = 5,
    FINTE_NODE_DIR = 6
} fnode_type_t;

/* Messages are deliberately small and fixed-size. Bulk data moves by passing
 * a capability to an arena, not by copying bytes through the kernel. */
typedef struct fmsg {
    uint32_t tag;
    uint16_t len;
    uint8_t cap_count;
    uint8_t flags;
    uint16_t caps[FINTE_MSG_CAPS];
    uint32_t reserved;
    uint8_t data[FINTE_MSG_DATA];
} fmsg_t;

#define FINTE_MSG_NONBLOCK 0x01u

typedef struct fstats {
    uint64_t syscalls;
    uint64_t flows_started;
    uint64_t flows_finished;
    uint64_t context_switches;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t caps_installed;
    uint64_t caps_dropped;
    uint64_t caps_denied;
    uint64_t fuel_burned;
    uint64_t epochs;
    uint64_t console_bytes;
    uint64_t deadlocks;
    uint32_t nodes_live;
    uint32_t channels_live;
    uint32_t arenas_live;
    uint32_t flows_live;
} fstats_t;

typedef struct fflow_stats {
    uint64_t syscalls;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t fuel_used;
    uint64_t slices;
    uint64_t denied;
} fflow_stats_t;

/* Small freestanding formatter. Supports %s %c %d %u %x %llu %llx %%.
 * Returns the number of characters written, excluding the terminator. */
int finte_format(char *out, size_t cap, const char *fmt, ...);
int finte_vformat(char *out, size_t cap, const char *fmt, va_list ap);

/* Freestanding helpers used by the core on every platform. */
void *finte_memcpy(void *dst, const void *src, size_t n);
void *finte_memset(void *dst, int value, size_t n);
int finte_memcmp(const void *a, const void *b, size_t n);
size_t finte_strlen(const char *s);
size_t finte_strlcpy(char *dst, const char *src, size_t cap);
int finte_strcmp(const char *a, const char *b);
int finte_strncmp(const char *a, const char *b, size_t n);

#endif /* FINTE_BASE_H */
