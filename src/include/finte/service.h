/* Capability-based user-service registry.
 *
 * A service registers an existing capability under a short name. Lookup
 * installs a fresh capability in the caller's table; the registry owns one
 * node reference for the lifetime of the entry. */
#ifndef FINTE_SERVICE_H
#define FINTE_SERVICE_H

#include "finte/base.h"
#include "finte/object.h"

struct fproc;

int finte_service_register(struct fproc *p, const char *name, uint16_t cap);
int finte_service_lookup(struct fproc *p, const char *name);
int finte_service_unregister(struct fproc *p, const char *name);
void finte_service_owner_exit(uint32_t pid);
uint32_t finte_service_count(void);

#endif /* FINTE_SERVICE_H */
