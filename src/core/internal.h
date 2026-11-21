#ifndef FINTE_CORE_INTERNAL_H
#define FINTE_CORE_INTERNAL_H

#include "finte/object.h"

fhandle_t finte_node_alloc(uint16_t type, uint32_t index, uint32_t aux);
void finte_node_release(fnode_t *n);
void finte_core_zero(void);

#endif /* FINTE_CORE_INTERNAL_H */
