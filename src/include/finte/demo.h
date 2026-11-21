#ifndef FINTE_DEMO_H
#define FINTE_DEMO_H

#include "finte/object.h"

typedef struct fdemo_result {
    uint64_t produced;
    uint64_t transformed;
    uint64_t consumed;
    uint64_t checksum;
    uint64_t denied;
    uint64_t expected_checksum;
    int32_t denied_status[2];
    int ok;
    int done;
} fdemo_result_t;

fret_t finte_demo_bootstrap(void);
const fdemo_result_t *finte_demo_result(void);

/* Shared Linux-persona demo: RAMFS + arena + a guest flow. */
fret_t finte_compat_bootstrap(void);

#endif /* FINTE_DEMO_H */
