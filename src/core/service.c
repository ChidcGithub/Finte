#include "finte/finte.h"
#include "finte/service.h"

#define FSERVICE_MAX 32
#define FSERVICE_NAME 24

typedef struct fservice {
    uint8_t used;
    char name[FSERVICE_NAME];
    fhandle_t handle;
    uint32_t rights;
    uint32_t owner;
} fservice_t;

static fservice_t g_services[FSERVICE_MAX];
static volatile int g_service_lock;

static uint64_t service_lock(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_service_lock);
    return flags;
}

static void service_unlock(uint64_t flags)
{
    hal_spin_unlock(&g_service_lock);
    hal_irq_restore(flags);
}

static int service_find(const char *name)
{
    for (uint32_t i = 0; i < FSERVICE_MAX; i++) {
        if (g_services[i].used &&
            finte_strcmp(g_services[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t finte_service_count(void)
{
    uint64_t flags = service_lock();
    uint32_t n = 0;
    for (uint32_t i = 0; i < FSERVICE_MAX; i++) {
        if (g_services[i].used) {
            n++;
        }
    }
    service_unlock(flags);
    return n;
}

int finte_service_register(fproc_t *p, const char *name, uint16_t cap)
{
    if (!p || !p->flow || !name || !*name ||
        finte_strlen(name) >= FSERVICE_NAME) {
        return FINTE_ERR_INVAL;
    }
    fcap_t c;
    int rc = finte_cap_check(p->flow, cap, 0, &c);
    if (rc < 0) {
        return rc;
    }
    uint64_t flags = service_lock();
    int slot = service_find(name);
    if (slot >= 0) {
        if (g_services[slot].owner != p->pid) {
            service_unlock(flags);
            return FINTE_ERR_STATE;
        }
        fhandle_t old = g_services[slot].handle;
        g_services[slot].handle = c.handle;
        g_services[slot].rights = c.rights;
        finte_node_ref(c.handle);
        service_unlock(flags);
        finte_node_unref(old);
        return 0;
    }
    for (uint32_t i = 0; i < FSERVICE_MAX; i++) {
        if (!g_services[i].used) {
            g_services[i].used = 1;
            finte_strlcpy(g_services[i].name, name, sizeof(g_services[i].name));
            g_services[i].handle = c.handle;
            g_services[i].rights = c.rights;
            g_services[i].owner = p->pid;
            finte_node_ref(c.handle);
            service_unlock(flags);
            return 0;
        }
    }
    service_unlock(flags);
    return FINTE_ERR_NOMEM;
}

int finte_service_lookup(fproc_t *p, const char *name)
{
    if (!p || !p->flow || !name) {
        return FINTE_ERR_INVAL;
    }
    uint64_t flags = service_lock();
    int slot = service_find(name);
    if (slot < 0) {
        service_unlock(flags);
        return FINTE_ERR_NOENT;
    }
    fhandle_t h = g_services[slot].handle;
    uint32_t rights = g_services[slot].rights;
    service_unlock(flags);
    return finte_cap_install(p->flow, h, rights);
}

int finte_service_unregister(fproc_t *p, const char *name)
{
    if (!p || !name) {
        return FINTE_ERR_INVAL;
    }
    uint64_t flags = service_lock();
    int slot = service_find(name);
    if (slot < 0 || g_services[slot].owner != p->pid) {
        service_unlock(flags);
        return FINTE_ERR_NOENT;
    }
    fhandle_t old = g_services[slot].handle;
    finte_memset(&g_services[slot], 0, sizeof(g_services[slot]));
    service_unlock(flags);
    finte_node_unref(old);
    return 0;
}

void finte_service_owner_exit(uint32_t pid)
{
    fhandle_t old[FSERVICE_MAX];
    uint32_t n = 0;
    uint64_t flags = service_lock();
    for (uint32_t i = 0; i < FSERVICE_MAX; i++) {
        if (g_services[i].used && g_services[i].owner == pid) {
            old[n++] = g_services[i].handle;
            finte_memset(&g_services[i], 0, sizeof(g_services[i]));
        }
    }
    service_unlock(flags);
    for (uint32_t i = 0; i < n; i++) {
        finte_node_unref(old[i]);
    }
}
