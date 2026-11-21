#include "finte/object.h"
#include "finte/verb.h"
#include "internal.h"

finte_core_t finte_core;

void finte_core_zero(void)
{
    finte_memset(&finte_core, 0, sizeof(finte_core));
    finte_core.rng = 0x9E3779B9u;
}

void finte_init(void)
{
    if (finte_core.initialized) {
        return;
    }
    finte_core_zero();
    finte_core.initialized = 1;
    finte_core.boot_cycles = hal_cycles();
    for (uint32_t i = 0; i < FINTE_MAX_NODES; i++) {
        finte_core.nodes[i].gen = 1;
    }
}

int finte_is_initialized(void)
{
    return finte_core.initialized;
}

void finte_reset(void)
{
    if (finte_sched_active()) {
        return;
    }
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        fflow_t *f = &finte_core.flows[i];
        if (f->in_use && f->ctx) {
            hal_ctx_destroy(f->ctx);
            f->ctx = NULL;
        }
    }
    for (uint32_t i = 0; i < FINTE_MAX_ARENAS; i++) {
        if (finte_core.arenas[i].in_use) {
            finte_arena_destroy(&finte_core.arenas[i]);
        }
    }
    finte_core_zero();
    finte_sched_reset();
    finte_core.initialized = 1;
    finte_core.boot_cycles = hal_cycles();
    for (uint32_t i = 0; i < FINTE_MAX_NODES; i++) {
        finte_core.nodes[i].gen = 1;
    }
}

fnode_t *finte_node_get(fhandle_t h)
{
    if (!finte_handle_valid(h)) {
        return NULL;
    }
    uint32_t slot = finte_handle_slot(h);
    if (slot >= FINTE_MAX_NODES) {
        return NULL;
    }
    fnode_t *n = &finte_core.nodes[slot];
    if (n->type == FINTE_NODE_FREE || n->gen != finte_handle_gen(h)) {
        return NULL;
    }
    return n;
}

fhandle_t finte_node_alloc(uint16_t type, uint32_t index, uint32_t aux)
{
    for (uint32_t i = 0; i < FINTE_MAX_NODES; i++) {
        fnode_t *n = &finte_core.nodes[i];
        if (n->type != FINTE_NODE_FREE) {
            continue;
        }
        if (n->gen == 0) {
            n->gen = 1;
        }
        n->type = type;
        n->flags = 0;
        n->refs = 0;
        n->index = index;
        n->aux = aux;
        return finte_handle_make(i, n->gen);
    }
    return FINTE_HANDLE_NONE;
}

void finte_node_ref(fhandle_t h)
{
    fnode_t *n = finte_node_get(h);
    if (n) {
        n->refs++;
    }
}

static void release_channel(fnode_t *n)
{
    fchan_t *ch = &finte_core.channels[n->index];
    finte_chan_release_queue(ch);
    ch->in_use = 0;
}

static void release_arena(fnode_t *n)
{
    finte_arena_destroy(&finte_core.arenas[n->index]);
}

static void release_flow(fnode_t *n)
{
    fflow_t *f = &finte_core.flows[n->index];
    if (f->in_use && (f->state == FINTE_FLOW_DEAD ||
                      f->state == FINTE_FLOW_FREE)) {
        if (f->ctx) {
            hal_ctx_destroy(f->ctx);
            f->ctx = NULL;
        }
        f->in_use = 0;
    }
}

static void release_file(fnode_t *n)
{
    finte_core.files[n->index].in_use = 0;
}

static void release_dir(fnode_t *n)
{
    fdir_t *d = &finte_core.dirs[n->index];
    for (uint16_t i = 0; i < d->count; i++) {
        finte_node_unref(d->ents[i].file);
    }
    d->count = 0;
    d->in_use = 0;
}

/* Indexed by FINTE_NODE_*; console and free nodes need no cleanup. */
static void (*const release_ops[])(fnode_t *) = {
    [FINTE_NODE_CHANNEL] = release_channel,
    [FINTE_NODE_ARENA] = release_arena,
    [FINTE_NODE_FLOW] = release_flow,
    [FINTE_NODE_FILE] = release_file,
    [FINTE_NODE_DIR] = release_dir,
};

void finte_node_release(fnode_t *n)
{
    if (!n || n->type == FINTE_NODE_FREE) {
        return;
    }
    if (n->type < FINTE_ARRAY_SIZE(release_ops) && release_ops[n->type]) {
        release_ops[n->type](n);
    }

    n->type = FINTE_NODE_FREE;
    n->flags = 0;
    n->refs = 0;
    n->index = 0;
    n->aux = 0;
    n->gen++;
    if (n->gen == 0) {
        n->gen = 1;
    }
}

void finte_node_unref(fhandle_t h)
{
    fnode_t *n = finte_node_get(h);
    if (!n) {
        return;
    }
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        finte_node_release(n);
    }
}

uint32_t finte_node_type(fhandle_t h)
{
    fnode_t *n = finte_node_get(h);
    return n ? n->type : FINTE_NODE_FREE;
}

uint32_t finte_node_refs(fhandle_t h)
{
    fnode_t *n = finte_node_get(h);
    return n ? n->refs : 0;
}

fhandle_t finte_console_create(const char *name)
{
    (void)name;
    return finte_node_alloc(FINTE_NODE_CONSOLE, 0, 0);
}

fhandle_t finte_channel_create(const char *name, uint16_t capacity)
{
    if (capacity == 0) {
        capacity = 1;
    }
    if (capacity > FINTE_CHAN_CAP) {
        capacity = FINTE_CHAN_CAP;
    }
    for (uint32_t i = 0; i < FINTE_MAX_CHANNELS; i++) {
        fchan_t *ch = &finte_core.channels[i];
        if (ch->in_use) {
            continue;
        }
        finte_memset(ch, 0, sizeof(*ch));
        ch->in_use = 1;
        ch->capacity = capacity;
        finte_strlcpy(ch->name, name ? name : "chan", sizeof(ch->name));
        fhandle_t h = finte_node_alloc(FINTE_NODE_CHANNEL, i, 0);
        if (!h) {
            ch->in_use = 0;
            return FINTE_HANDLE_NONE;
        }
        ch->self = h;
        return h;
    }
    return FINTE_HANDLE_NONE;
}

fhandle_t finte_arena_create(const char *name, size_t bytes)
{
    if (bytes == 0) {
        bytes = 64 * 1024;
    }
    bytes = (bytes + 4095u) & ~(size_t)4095u;
    for (uint32_t i = 0; i < FINTE_MAX_ARENAS; i++) {
        farena_t *a = &finte_core.arenas[i];
        if (a->in_use) {
            continue;
        }
        uint8_t *base = (uint8_t *)hal_pages_alloc(bytes);
        if (!base) {
            return FINTE_HANDLE_NONE;
        }
        finte_memset(base, 0, bytes);
        finte_memset(a, 0, sizeof(*a));
        a->in_use = 1;
        a->base = base;
        a->size = bytes;
        fblock_t *b = (fblock_t *)base;
        b->size = bytes;
        b->next = NULL;
        b->free = 1;
        b->magic = 0xF1A7E000u;
        a->free_list = b;
        finte_strlcpy(a->name, name ? name : "arena", sizeof(a->name));
        fhandle_t h = finte_node_alloc(FINTE_NODE_ARENA, i, 0);
        if (!h) {
            hal_pages_free(base, bytes);
            finte_memset(a, 0, sizeof(*a));
            return FINTE_HANDLE_NONE;
        }
        a->self = h;
        return h;
    }
    return FINTE_HANDLE_NONE;
}

fhandle_t finte_file_create(const char *name, const void *data, size_t size)
{
    for (uint32_t i = 0; i < FINTE_MAX_FILES; i++) {
        ffile_t *file = &finte_core.files[i];
        if (file->in_use) {
            continue;
        }
        finte_memset(file, 0, sizeof(*file));
        file->in_use = 1;
        file->backend = 0;
        file->rights = FINTE_R_READ;
        file->data = (const uint8_t *)data;
        file->size = size;
        finte_strlcpy(file->name, name ? name : "file", sizeof(file->name));
        return finte_node_alloc(FINTE_NODE_FILE, i, 0);
    }
    return FINTE_HANDLE_NONE;
}

fhandle_t finte_dir_create(const char *name)
{
    for (uint32_t i = 0; i < FINTE_MAX_DIRS; i++) {
        fdir_t *d = &finte_core.dirs[i];
        if (d->in_use) {
            continue;
        }
        finte_memset(d, 0, sizeof(*d));
        d->in_use = 1;
        finte_strlcpy(d->name, name ? name : "/", sizeof(d->name));
        fhandle_t h = finte_node_alloc(FINTE_NODE_DIR, i, 0);
        if (!h) {
            d->in_use = 0;
            return FINTE_HANDLE_NONE;
        }
        d->self = h;
        return h;
    }
    return FINTE_HANDLE_NONE;
}

fret_t finte_dir_add(fhandle_t dir, const char *name, fhandle_t file)
{
    fnode_t *dn = finte_node_get(dir);
    fnode_t *fn = finte_node_get(file);
    if (!dn || dn->type != FINTE_NODE_DIR) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    if (!fn || fn->type != FINTE_NODE_FILE) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    if (!name || !*name) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    fdir_t *d = &finte_core.dirs[dn->index];
    for (uint16_t i = 0; i < d->count; i++) {
        if (finte_strcmp(d->ents[i].name, name) == 0) {
            return finte_ret(FINTE_ERR_EXIST, 0);
        }
    }
    if (d->count >= FINTE_DIR_ENTS) {
        return finte_ret(FINTE_ERR_LIMIT, 0);
    }
    finte_strlcpy(d->ents[d->count].name, name, sizeof(d->ents[0].name));
    d->ents[d->count].file = file;
    finte_node_ref(file);
    d->count++;
    return finte_ret_ok((uint64_t)d->count - 1u);
}

fstats_t finte_get_stats(void)
{
    fstats_t s = finte_core.stats;
    s.nodes_live = 0;
    s.flows_live = 0;
    s.channels_live = 0;
    s.arenas_live = 0;
    for (uint32_t i = 0; i < FINTE_MAX_NODES; i++) {
        if (finte_core.nodes[i].type != FINTE_NODE_FREE) {
            s.nodes_live++;
        }
    }
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        if (finte_core.flows[i].in_use &&
            finte_core.flows[i].state != FINTE_FLOW_DEAD) {
            s.flows_live++;
        }
    }
    for (uint32_t i = 0; i < FINTE_MAX_CHANNELS; i++) {
        if (finte_core.channels[i].in_use) {
            s.channels_live++;
        }
    }
    for (uint32_t i = 0; i < FINTE_MAX_ARENAS; i++) {
        if (finte_core.arenas[i].in_use) {
            s.arenas_live++;
        }
    }
    return s;
}

static void dump_line(const char *text)
{
    hal_console_write(text, finte_strlen(text));
}

void finte_dump(const char *title)
{
    char buf[256];
    fstats_t s = finte_get_stats();
    dump_line("\n-- ");
    dump_line(title ? title : "finte");
    dump_line(" --\n");
    finte_format(buf, sizeof(buf),
                 "nodes=%u flows=%u channels=%u arenas=%u\n",
                 (unsigned)s.nodes_live, (unsigned)s.flows_live,
                 (unsigned)s.channels_live, (unsigned)s.arenas_live);
    dump_line(buf);
    finte_format(buf, sizeof(buf),
                 "syscalls=%llu msgs=%llu/%llu switches=%llu epochs=%llu\n",
                 (unsigned long long)s.syscalls,
                 (unsigned long long)s.messages_sent,
                 (unsigned long long)s.messages_received,
                 (unsigned long long)s.context_switches,
                 (unsigned long long)s.epochs);
    dump_line(buf);
    finte_format(buf, sizeof(buf), "fuel=%llu console=%llu\n",
                 (unsigned long long)s.fuel_burned,
                 (unsigned long long)s.console_bytes);
    dump_line(buf);
}
