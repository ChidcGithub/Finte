#include "finte/object.h"
#include "finte/verb.h"

int finte_cap_free_slots(fflow_t *f)
{
    if (!f) {
        return 0;
    }
    int n = 0;
    for (uint32_t i = 0; i < FINTE_MAX_CAPS; i++) {
        if (!finte_handle_valid(f->caps[i].handle)) {
            n++;
        }
    }
    return n;
}

int finte_cap_install(fflow_t *f, fhandle_t h, uint32_t rights)
{
    if (!f || !finte_node_get(h)) {
        return FINTE_ERR_NOENT;
    }
    for (uint32_t i = 0; i < FINTE_MAX_CAPS; i++) {
        if (finte_handle_valid(f->caps[i].handle)) {
            continue;
        }
        f->caps[i].handle = h;
        f->caps[i].rights = rights;
        f->caps[i].flags = 0;
        finte_node_ref(h);
        if (i + 1 > f->cap_count) {
            f->cap_count = (uint16_t)(i + 1);
        }
        finte_core.stats.caps_installed++;
        return (int)i;
    }
    return FINTE_ERR_LIMIT;
}

int finte_cap_check(fflow_t *f, uint16_t cap, uint32_t need, fcap_t *out)
{
    if (!f || cap >= FINTE_MAX_CAPS) {
        finte_core.stats.caps_denied++;
        return FINTE_ERR_INVAL;
    }
    fcap_t *c = &f->caps[cap];
    if (!finte_handle_valid(c->handle)) {
        finte_core.stats.caps_denied++;
        return FINTE_ERR_NOENT;
    }
    fnode_t *n = finte_node_get(c->handle);
    if (!n) {
        finte_core.stats.caps_denied++;
        c->handle = FINTE_HANDLE_NONE;
        return FINTE_ERR_NOENT;
    }
    if ((c->rights & need) != need) {
        finte_core.stats.caps_denied++;
        f->stats.denied++;
        return FINTE_ERR_PERM;
    }
    if (out) {
        *out = *c;
    }
    return 0;
}

int finte_cap_drop_index(fflow_t *f, uint16_t cap)
{
    if (!f || cap >= FINTE_MAX_CAPS) {
        return FINTE_ERR_INVAL;
    }
    fcap_t *c = &f->caps[cap];
    if (!finte_handle_valid(c->handle)) {
        return FINTE_ERR_NOENT;
    }
    fhandle_t h = c->handle;
    c->handle = FINTE_HANDLE_NONE;
    c->rights = 0;
    finte_core.stats.caps_dropped++;
    finte_node_unref(h);
    return 0;
}

void finte_cap_drop_all(fflow_t *f)
{
    if (!f) {
        return;
    }
    for (uint32_t i = 0; i < FINTE_MAX_CAPS; i++) {
        if (finte_handle_valid(f->caps[i].handle)) {
            finte_cap_drop_index(f, (uint16_t)i);
        }
    }
    f->cap_count = 0;
}
