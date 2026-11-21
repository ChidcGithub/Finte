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

static int cap_install(fflow_t *f, fhandle_t h, uint32_t rights,
                       uint32_t flags, uint64_t offset, uint64_t length)
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
        f->caps[i].flags = flags;
        f->caps[i].offset = offset;
        f->caps[i].length = length;
        finte_node_ref(h);
        if (i + 1 > f->cap_count) {
            f->cap_count = (uint16_t)(i + 1);
        }
        finte_core.stats.caps_installed++;
        return (int)i;
    }
    return FINTE_ERR_LIMIT;
}

int finte_cap_install(fflow_t *f, fhandle_t h, uint32_t rights)
{
    return cap_install(f, h, rights, 0, 0, 0);
}

int finte_cap_install_scoped(fflow_t *f, fhandle_t h, uint32_t rights,
                             uint64_t offset, uint64_t length)
{
    if (length == 0 || offset + length < offset) {
        return FINTE_ERR_INVAL;
    }
    return cap_install(f, h, rights, FINTE_CAP_SCOPED, offset, length);
}

int finte_cap_install_cap(fflow_t *f, const fcap_t *cap)
{
    if (!cap) {
        return FINTE_ERR_INVAL;
    }
    return cap_install(f, cap->handle, cap->rights, cap->flags,
                       cap->offset, cap->length);
}

int finte_cap_install_arena_pages(fflow_t *f, fhandle_t arena,
                                  uint32_t first_page, uint32_t pages,
                                  uint32_t rights)
{
    fnode_t *n = finte_node_get(arena);
    if (!n || n->type != FINTE_NODE_ARENA) {
        return FINTE_ERR_TYPE;
    }
    farena_t *a = &finte_core.arenas[n->index];
    uint64_t offset = (uint64_t)first_page * 4096u;
    uint64_t length = (uint64_t)pages * 4096u;
    if (pages == 0 || offset / 4096u != first_page ||
        offset + length < offset || offset + length > a->size) {
        return FINTE_ERR_RANGE;
    }
    fret_t r = finte_arena_reserve(a, offset, length);
    if (finte_failed(r)) {
        return r.status;
    }
    return finte_cap_install_scoped(f, arena, rights, offset, length);
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
    c->flags = 0;
    c->offset = 0;
    c->length = 0;
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
