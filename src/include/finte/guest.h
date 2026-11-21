#ifndef FINTE_GUEST_H
#define FINTE_GUEST_H

#include "finte/object.h"

typedef struct fguest_result {
    int ok;
    int64_t bytes_read;
    int64_t pid;
    int64_t fd;
    uint64_t brk_before;
    uint64_t brk_after;
    uint64_t monotonic_ns;
    char sysname[16];
    char machine[16];
} fguest_result_t;

extern const fprogram_t finte_linux_guest_program;
const fguest_result_t *finte_guest_result(void);

#endif /* FINTE_GUEST_H */
