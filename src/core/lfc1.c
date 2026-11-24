/* lfc1: compact, read-optimized filesystem.
 *
 * v1 is read-only. v2 adds copy-on-write updates and a metadata journal:
 * the mounted tree lives in RAM, sync appends a complete new tree, commits
 * a journal record containing the new superblock, then updates the
 * superblock. Mount replays the newest valid record if it is ahead of the
 * superblock; invalid/torn records are ignored. The journal is a reserved
 * tail region, so the live tree never overlaps it. */
#include "fs_internal.h"

#define LFC1_MAGIC 0x3143464Cu
#define LFC1_VERSION 2
#define LFC1_DIR_MAGIC 0x3144524Cu
#define LFC1_DIR_MAX (64u * 1024u)
#define LFC1_NAME_MAX 255u
#define LFC1_MAX_NODES 512u
#define LFC1_MAX_DEPTH 24u
#define LFC1_MAX_DATA (8u * 1024u * 1024u)
#define LFC1_ENTRY_DIR 1u
#define LFC1_JOURNAL_MAGIC 0x4A43464Cu
#define LFC1_JOURNAL_COMMIT 0x54494D43u
#define LFC1_JOURNAL_SLOTS 2u
#define LFC1_JOURNAL_SLOT_SECTORS 2u

typedef struct __attribute__((packed)) lfc1_sb {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t block_size;
    uint32_t total_sectors;
    uint32_t root_start;
    uint32_t root_size;
    uint32_t data_start;
    char label[32];
    uint8_t uuid[16];
    uint64_t created_ns;
    uint32_t next_free;
    uint32_t generation;
    uint32_t free_sectors;
    uint32_t journal_start;
    uint32_t journal_sectors;
    uint32_t journal_sequence;
    uint32_t journal_flags;
    uint8_t reserved[396];
    uint32_t checksum;
} lfc1_sb_t;

_Static_assert(sizeof(lfc1_sb_t) == 512, "lfc1 superblock must be 512 bytes");

typedef struct __attribute__((packed)) lfc1_journal {
    uint32_t magic;
    uint32_t sequence;
    uint32_t sb_size;
    uint32_t sb_crc;
    uint8_t sb[512];
    uint32_t commit;
    uint32_t crc;
} lfc1_journal_t;

_Static_assert(sizeof(lfc1_journal_t) <= 1024, "lfc1 journal record too large");

typedef struct __attribute__((packed)) lfc1_dhdr {
    uint32_t magic;
    uint32_t count;
    uint32_t names_off;
    uint32_t flags;
} lfc1_dhdr_t;

typedef struct __attribute__((packed)) lfc1_de {
    uint32_t name_off;
    uint32_t start;
    uint32_t size;
    uint8_t flags;
    uint8_t name_len;
    uint16_t reserved;
} lfc1_de_t;

typedef struct lfc1_node {
    uint8_t used;
    uint8_t is_dir;
    uint16_t pad;
    uint32_t size;
    uint32_t start;
    uint32_t blob_size;
    char name[LFC1_NAME_MAX + 1];
    uint8_t *data;
    struct lfc1_node *parent;
    struct lfc1_node *child;
    struct lfc1_node *next;
} lfc1_node_t;

typedef struct lfc1_ctx {
    fdfs_t *fs;
    lfc1_sb_t sb;
    int writable;
    int dirty;
    uint32_t node_count;
    uint32_t data_bytes;
    lfc1_node_t nodes[LFC1_MAX_NODES];
    lfc1_node_t *sort_buf[LFC1_MAX_NODES];
    uint8_t dirbuf[LFC1_DIR_MAX];
} lfc1_ctx_t;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t align4k(uint32_t sec)
{
    if (sec > 0xFFFFFFF0u) {
        return 0xFFFFFFFFu;
    }
    return (sec + 7u) & ~7u;
}

static int name_eq(const char *a, const char *b)
{
    return finte_strcmp(a, b) == 0;
}

static lfc1_node_t *node_child(lfc1_node_t *dir, const char *name)
{
    for (lfc1_node_t *n = dir->child; n; n = n->next) {
        if (name_eq(n->name, name)) {
            return n;
        }
    }
    return NULL;
}

static void node_attach(lfc1_node_t *parent, lfc1_node_t *node)
{
    node->parent = parent;
    node->next = NULL;
    if (!parent->child) {
        parent->child = node;
        return;
    }
    lfc1_node_t *p = parent->child;
    while (p->next) {
        p = p->next;
    }
    p->next = node;
}

static void node_detach(lfc1_node_t *node)
{
    lfc1_node_t *p = node->parent;
    if (!p) {
        return;
    }
    if (p->child == node) {
        p->child = node->next;
    } else {
        lfc1_node_t *q = p->child;
        while (q && q->next != node) {
            q = q->next;
        }
        if (q) {
            q->next = node->next;
        }
    }
    node->next = NULL;
    node->parent = NULL;
}

static void node_free_data(lfc1_ctx_t *c, lfc1_node_t *n)
{
    if (n->data) {
        uint32_t bytes = (n->size + 4095u) & ~4095u;
        hal_pages_free(n->data, bytes ? bytes : 4096u);
        if (c->data_bytes >= n->size) {
            c->data_bytes -= n->size;
        } else {
            c->data_bytes = 0;
        }
        n->data = NULL;
    }
    n->size = 0;
}

static void node_free_tree(lfc1_ctx_t *c, lfc1_node_t *n)
{
    lfc1_node_t *ch = n->child;
    while (ch) {
        lfc1_node_t *next = ch->next;
        node_free_tree(c, ch);
        ch = next;
    }
    node_free_data(c, n);
    n->child = NULL;
    n->used = 0;
    if (c->node_count) {
        c->node_count--;
    }
}

static lfc1_node_t *node_alloc(lfc1_ctx_t *c, const char *name, int is_dir)
{
    if (c->node_count >= LFC1_MAX_NODES) {
        return NULL;
    }
    size_t len = finte_strlen(name);
    if (len == 0 || len > LFC1_NAME_MAX) {
        return NULL;
    }
    for (uint32_t i = 0; i < LFC1_MAX_NODES; i++) {
        lfc1_node_t *n = &c->nodes[i];
        if (n->used) {
            continue;
        }
        finte_memset(n, 0, sizeof(*n));
        n->used = 1;
        n->is_dir = (uint8_t)is_dir;
        finte_memcpy(n->name, name, len + 1);
        c->node_count++;
        return n;
    }
    return NULL;
}

static int load_dir(lfc1_ctx_t *c, lfc1_node_t *dir, uint32_t start,
                    uint32_t size, uint32_t depth)
{
    if (!dir->is_dir || depth > LFC1_MAX_DEPTH ||
        size < sizeof(lfc1_dhdr_t) || size > LFC1_DIR_MAX) {
        return FINTE_ERR_IO;
    }
    dir->size = size;
    if ((uint64_t)start * 512u + size >
        (uint64_t)c->sb.total_sectors * 512u) {
        return FINTE_ERR_IO;
    }
    uint8_t *buf = (uint8_t *)hal_pages_alloc((size + 4095u) & ~4095u);
    if (!buf) {
        return FINTE_ERR_NOMEM;
    }
    if (fdfs_read_bytes(c->fs, (uint64_t)start * 512u, buf, size) != 0) {
        hal_pages_free(buf, (size + 4095u) & ~4095u);
        return FINTE_ERR_IO;
    }
    const lfc1_dhdr_t *h = (const lfc1_dhdr_t *)buf;
    if (h->magic != LFC1_DIR_MAGIC || h->count > LFC1_DIR_MAX / 16u) {
        hal_pages_free(buf, (size + 4095u) & ~4095u);
        return FINTE_ERR_IO;
    }
    uint64_t entries_end = sizeof(lfc1_dhdr_t) +
                           (uint64_t)h->count * sizeof(lfc1_de_t);
    if (entries_end > size || h->names_off < entries_end ||
        h->names_off > size) {
        hal_pages_free(buf, (size + 4095u) & ~4095u);
        return FINTE_ERR_IO;
    }
    for (uint32_t i = 0; i < h->count; i++) {
        const lfc1_de_t *e = (const lfc1_de_t *)(buf + sizeof(*h) +
                                                 (uint64_t)i * sizeof(*e));
        if (e->name_len == 0 ||
            (uint64_t)h->names_off + e->name_off + e->name_len > size) {
            hal_pages_free(buf, (size + 4095u) & ~4095u);
            return FINTE_ERR_IO;
        }
        char name[LFC1_NAME_MAX + 1];
        finte_memcpy(name, buf + h->names_off + e->name_off, e->name_len);
        name[e->name_len] = 0;
        if (node_child(dir, name)) {
            hal_pages_free(buf, (size + 4095u) & ~4095u);
            return FINTE_ERR_IO;
        }
        lfc1_node_t *n = node_alloc(c, name, (e->flags & LFC1_ENTRY_DIR) != 0);
        if (!n) {
            hal_pages_free(buf, (size + 4095u) & ~4095u);
            return FINTE_ERR_NOMEM;
        }
        node_attach(dir, n);
        n->size = e->size;
        n->start = e->start;
        if ((uint64_t)e->start * 512u + e->size >
            (uint64_t)c->sb.total_sectors * 512u) {
            hal_pages_free(buf, (size + 4095u) & ~4095u);
            return FINTE_ERR_IO;
        }
        if (n->is_dir) {
            int rc = load_dir(c, n, e->start, e->size, depth + 1);
            if (rc != 0) {
                hal_pages_free(buf, (size + 4095u) & ~4095u);
                return rc;
            }
        } else if (e->size) {
            if (e->size > LFC1_MAX_DATA ||
                c->data_bytes + e->size > LFC1_MAX_DATA) {
                hal_pages_free(buf, (size + 4095u) & ~4095u);
                return FINTE_ERR_NOMEM;
            }
            uint32_t alloc = (e->size + 4095u) & ~4095u;
            n->data = (uint8_t *)hal_pages_alloc(alloc);
            if (!n->data) {
                hal_pages_free(buf, (size + 4095u) & ~4095u);
                return FINTE_ERR_NOMEM;
            }
            if (fdfs_read_bytes(c->fs, (uint64_t)e->start * 512u,
                                n->data, e->size) != 0) {
                hal_pages_free(buf, (size + 4095u) & ~4095u);
                return FINTE_ERR_IO;
            }
            c->data_bytes += e->size;
        }
    }
    hal_pages_free(buf, (size + 4095u) & ~4095u);
    return 0;
}

static int path_split(const char *rel, char *parent, size_t pcap,
                      char *name, size_t ncap)
{
    if (!rel || !*rel) {
        return FINTE_ERR_INVAL;
    }
    size_t len = finte_strlen(rel);
    if (rel[len - 1] == '/') {
        return FINTE_ERR_INVAL;
    }
    const char *slash = NULL;
    for (const char *p = rel; *p; p++) {
        if (*p == '/') {
            slash = p;
        }
    }
    const char *leaf = slash ? slash + 1 : rel;
    size_t nl = finte_strlen(leaf);
    if (nl == 0 || nl > LFC1_NAME_MAX || nl + 1 > ncap) {
        return FINTE_ERR_INVAL;
    }
    if (slash) {
        size_t pl = (size_t)(slash - rel);
        if (pl + 1 > pcap) {
            return FINTE_ERR_INVAL;
        }
        finte_memcpy(parent, rel, pl);
        parent[pl] = 0;
    } else {
        parent[0] = 0;
    }
    finte_memcpy(name, leaf, nl + 1);
    return 0;
}

static int resolve(lfc1_ctx_t *c, const char *rel, lfc1_node_t **out)
{
    lfc1_node_t *cur = &c->nodes[0];
    const char *p = rel;
    char name[LFC1_NAME_MAX + 1];
    for (;;) {
        int more = fdfs_split_next(&p, name, sizeof(name));
        if (more < 0) {
            return more;
        }
        if (more == 0) {
            *out = cur;
            return 0;
        }
        if (finte_strcmp(name, ".") == 0) {
            continue;
        }
        if (finte_strcmp(name, "..") == 0) {
            if (cur->parent) {
                cur = cur->parent;
            }
            continue;
        }
        if (!cur->is_dir) {
            return FINTE_ERR_TYPE;
        }
        lfc1_node_t *n = node_child(cur, name);
        if (!n) {
            return FINTE_ERR_NOENT;
        }
        cur = n;
    }
}

static int resolve_parent(lfc1_ctx_t *c, const char *rel,
                          lfc1_node_t **parent, char *name, size_t ncap)
{
    char pbuf[FDISK_PATH_MAX];
    int rc = path_split(rel, pbuf, sizeof(pbuf), name, ncap);
    if (rc != 0) {
        return rc;
    }
    return resolve(c, pbuf, parent);
}

static uint32_t blob_size(const lfc1_node_t *dir)
{
    uint32_t count = 0;
    uint32_t names = 0;
    for (const lfc1_node_t *n = dir->child; n; n = n->next) {
        count++;
        names += (uint32_t)finte_strlen(n->name);
    }
    return 16u + count * 16u + names;
}

static void sort_children(lfc1_node_t *dir, lfc1_node_t **kids, uint32_t count)
{
    uint32_t i = 0;
    for (lfc1_node_t *n = dir->child; n; n = n->next) {
        kids[i++] = n;
    }
    for (uint32_t a = 1; a < count; a++) {
        lfc1_node_t *key = kids[a];
        uint32_t b = a;
        while (b > 0 && finte_strcmp(kids[b - 1]->name, key->name) > 0) {
            kids[b] = kids[b - 1];
            b--;
        }
        kids[b] = key;
    }
}

static int build_blob(lfc1_ctx_t *c, lfc1_node_t *dir, uint8_t *buf,
                      uint32_t *out_len)
{
    uint32_t count = 0;
    for (lfc1_node_t *n = dir->child; n; n = n->next) {
        count++;
    }
    if (count > LFC1_MAX_NODES - 1) {
        return FINTE_ERR_LIMIT;
    }
    sort_children(dir, c->sort_buf, count);
    uint32_t names_off = 16u + count * 16u;
    uint32_t names = 0;
    for (uint32_t i = 0; i < count; i++) {
        names += (uint32_t)finte_strlen(c->sort_buf[i]->name);
    }
    uint32_t len = names_off + names;
    if (len > LFC1_DIR_MAX) {
        return FINTE_ERR_LIMIT;
    }
    lfc1_dhdr_t *h = (lfc1_dhdr_t *)buf;
    h->magic = LFC1_DIR_MAGIC;
    h->count = count;
    h->names_off = names_off;
    h->flags = 0;
    uint32_t name_at = 0;
    for (uint32_t i = 0; i < count; i++) {
        lfc1_node_t *n = c->sort_buf[i];
        lfc1_de_t *e = (lfc1_de_t *)(buf + 16u + i * 16u);
        size_t nl = finte_strlen(n->name);
        e->name_off = name_at;
        e->start = n->start;
        e->size = n->is_dir ? n->blob_size : n->size;
        e->flags = n->is_dir ? LFC1_ENTRY_DIR : 0;
        e->name_len = (uint8_t)nl;
        e->reserved = 0;
        finte_memcpy(buf + names_off + name_at, n->name, nl);
        name_at += (uint32_t)nl;
    }
    *out_len = len;
    return 0;
}

static int assign_dirs(lfc1_node_t *n, uint32_t *cur)
{
    n->blob_size = blob_size(n);
    if (n->blob_size > LFC1_DIR_MAX) {
        return FINTE_ERR_LIMIT;
    }
    n->start = align4k(*cur);
    uint32_t need = align4k(n->start + (n->blob_size + 511u) / 512u);
    if (need == 0xFFFFFFFFu || need < n->start) {
        return FINTE_ERR_RANGE;
    }
    *cur = need;
    for (lfc1_node_t *ch = n->child; ch; ch = ch->next) {
        if (ch->is_dir) {
            int rc = assign_dirs(ch, cur);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

static int assign_files(lfc1_node_t *n, uint32_t *cur)
{
    for (lfc1_node_t *ch = n->child; ch; ch = ch->next) {
        if (ch->is_dir) {
            int rc = assign_files(ch, cur);
            if (rc != 0) {
                return rc;
            }
        } else {
            ch->start = align4k(*cur);
            uint32_t need = align4k(ch->start + (ch->size + 511u) / 512u);
            if (need == 0xFFFFFFFFu || need < ch->start) {
                return FINTE_ERR_RANGE;
            }
            *cur = need;
        }
    }
    return 0;
}

static int write_dirs(lfc1_ctx_t *c, lfc1_node_t *n)
{
    uint32_t len = 0;
    int rc = build_blob(c, n, c->dirbuf, &len);
    if (rc != 0) {
        return rc;
    }
    if (fdfs_write_bytes(c->fs, (uint64_t)n->start * 512u,
                         c->dirbuf, len) != 0) {
        return FINTE_ERR_IO;
    }
    for (lfc1_node_t *ch = n->child; ch; ch = ch->next) {
        if (ch->is_dir) {
            rc = write_dirs(c, ch);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

static int write_files(lfc1_ctx_t *c, lfc1_node_t *n)
{
    for (lfc1_node_t *ch = n->child; ch; ch = ch->next) {
        if (ch->is_dir) {
            int rc = write_files(c, ch);
            if (rc != 0) {
                return rc;
            }
        } else if (ch->size) {
            if (fdfs_write_bytes(c->fs, (uint64_t)ch->start * 512u,
                                 ch->data, ch->size) != 0) {
                return FINTE_ERR_IO;
            }
        }
    }
    return 0;
}

static int lfc1_journal_valid(const uint8_t *rec, uint32_t *seq,
                              uint8_t *sb_out)
{
    const lfc1_journal_t *j = (const lfc1_journal_t *)(const void *)rec;
    if (j->magic != LFC1_JOURNAL_MAGIC || j->sb_size != 512 ||
        j->commit != LFC1_JOURNAL_COMMIT) {
        return -1;
    }
    if (fnv1a(rec, 532) != j->crc ||
        fnv1a(j->sb, 508) != j->sb_crc) {
        return -1;
    }
    const lfc1_sb_t *s = (const lfc1_sb_t *)(const void *)j->sb;
    if (s->magic != LFC1_MAGIC || (s->version != 1 && s->version != 2)) {
        return -1;
    }
    if (s->flags & 1u) {
        if (fnv1a(j->sb, 508) != s->checksum) {
            return -1;
        }
    }
    if (seq) {
        *seq = j->sequence;
    }
    if (sb_out) {
        finte_memcpy(sb_out, j->sb, 512);
    }
    return 0;
}

static int lfc1_journal_replay(fdfs_t *fs, uint8_t *sbraw)
{
    const lfc1_sb_t *s = (const lfc1_sb_t *)(const void *)sbraw;
    if (s->journal_start == 0 || s->journal_sectors < 4 ||
        (uint64_t)s->journal_start + s->journal_sectors >
            s->total_sectors ||
        (uint64_t)s->journal_start * 512u > fs->sectors * 512u) {
        return 0;
    }
    uint32_t best = s->generation;
    uint8_t best_sb[512];
    int found = 0;
    for (uint32_t slot = 0; slot < LFC1_JOURNAL_SLOTS; slot++) {
        uint8_t rec[1024];
        uint64_t off = ((uint64_t)s->journal_start +
                        (uint64_t)slot * LFC1_JOURNAL_SLOT_SECTORS) * 512u;
        if (fdfs_read_bytes(fs, off, rec, sizeof(rec)) != 0) {
            continue;
        }
        uint32_t seq = 0;
        uint8_t sb[512];
        if (lfc1_journal_valid(rec, &seq, sb) != 0 || seq <= best) {
            continue;
        }
        best = seq;
        finte_memcpy(best_sb, sb, sizeof(best_sb));
        found = 1;
    }
    if (!found) {
        return 0;
    }
    if (fdfs_write_sectors(fs, 0, best_sb, 1) != 0) {
        return FINTE_ERR_IO;
    }
    finte_memcpy(sbraw, best_sb, 512);
    return 0;
}

static int lfc1_journal_commit(lfc1_ctx_t *c, const lfc1_sb_t *nsb)
{
    if (c->sb.journal_start == 0 || c->sb.journal_sectors < 4) {
        return 0;
    }
    uint8_t rec[1024];
    finte_memset(rec, 0, sizeof(rec));
    lfc1_journal_t *j = (lfc1_journal_t *)(void *)rec;
    j->magic = LFC1_JOURNAL_MAGIC;
    j->sequence = nsb->generation;
    j->sb_size = 512;
    j->sb_crc = fnv1a((const uint8_t *)nsb, 508);
    finte_memcpy(j->sb, nsb, 512);
    j->commit = LFC1_JOURNAL_COMMIT;
    j->crc = fnv1a(rec, 532);
    uint32_t slot = nsb->generation & 1u;
    uint64_t off = ((uint64_t)c->sb.journal_start +
                    (uint64_t)slot * LFC1_JOURNAL_SLOT_SECTORS) * 512u;
    if (fdfs_write_bytes(c->fs, off, rec, sizeof(rec)) != 0) {
        return FINTE_ERR_IO;
    }
    return 0;
}

static int lfc1_sync(void *ctx)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c->writable) {
        return 0;
    }
    if (!c->dirty) {
        return 0;
    }
    lfc1_node_t *root = &c->nodes[0];
    uint32_t limit = c->sb.journal_start ? c->sb.journal_start
                                         : c->sb.total_sectors;
    uint32_t cur = align4k(c->sb.next_free);
    if (cur == 0xFFFFFFFFu || cur >= limit) {
        return FINTE_ERR_NOMEM;
    }
    int rc = assign_dirs(root, &cur);
    if (rc == 0) {
        rc = assign_files(root, &cur);
    }
    uint32_t next = align4k(cur);
    if (rc != 0 || next == 0xFFFFFFFFu || next > limit) {
        return FINTE_ERR_NOMEM;
    }
    rc = write_dirs(c, root);
    if (rc == 0) {
        rc = write_files(c, root);
    }
    if (rc != 0) {
        return rc;
    }
    lfc1_sb_t nsb = c->sb;
    nsb.root_start = root->start;
    nsb.root_size = root->blob_size;
    nsb.next_free = next;
    nsb.generation++;
    nsb.free_sectors = limit - next;
    nsb.journal_sequence = nsb.generation;
    nsb.checksum = fnv1a((const uint8_t *)&nsb, 508);
    if (lfc1_journal_commit(c, &nsb) != 0) {
        return FINTE_ERR_IO;
    }
    uint8_t sb[512];
    finte_memcpy(sb, &nsb, sizeof(sb));
    if (fdfs_write_sectors(c->fs, 0, sb, 1) != 0) {
        return FINTE_ERR_IO;
    }
    c->sb = nsb;
    c->dirty = 0;
    return 0;
}

static int lfc1_mount(fdfs_t *fs)
{
    uint8_t sbraw[512];
    if (fdfs_read_sectors(fs, 0, sbraw, 1) != 0) {
        return FINTE_ERR_IO;
    }
    const lfc1_sb_t *s = (const lfc1_sb_t *)sbraw;
    if (s->magic != LFC1_MAGIC ||
        (s->version != 1 && s->version != 2)) {
        return FINTE_ERR_TYPE;
    }
    if (s->version >= 2 && s->journal_start != 0) {
        int jr = lfc1_journal_replay(fs, sbraw);
        if (jr != 0) {
            return jr;
        }
        s = (const lfc1_sb_t *)sbraw;
    }
    if (s->flags & 1u) {
        if (fnv1a(sbraw, 508) != rd32(sbraw + 508)) {
            return FINTE_ERR_IO;
        }
    }
    uint32_t bs = s->block_size;
    if (bs < 512 || bs > 65536 || (bs & (bs - 1)) != 0 ||
        s->total_sectors == 0 || s->total_sectors > fs->sectors ||
        s->root_size < sizeof(lfc1_dhdr_t) || s->root_size > LFC1_DIR_MAX ||
        (uint64_t)s->root_start * 512u + s->root_size >
            (uint64_t)s->total_sectors * 512u ||
        (s->journal_start != 0 &&
         (s->journal_sectors < 4 ||
          (uint64_t)s->journal_start + s->journal_sectors >
              s->total_sectors))) {
        return FINTE_ERR_IO;
    }
    lfc1_ctx_t *c = (lfc1_ctx_t *)hal_pages_alloc(sizeof(*c));
    if (!c) {
        return FINTE_ERR_NOMEM;
    }
    finte_memset(c, 0, sizeof(*c));
    c->fs = fs;
    finte_memcpy(&c->sb, s, sizeof(c->sb));
    c->writable = (s->version >= 2);
    c->node_count = 1;
    c->nodes[0].used = 1;
    c->nodes[0].is_dir = 1;
    c->nodes[0].name[0] = 0;
    int rc = load_dir(c, &c->nodes[0], c->sb.root_start, c->sb.root_size, 0);
    if (rc != 0) {
        node_free_tree(c, &c->nodes[0]);
        hal_pages_free(c, sizeof(*c));
        return rc;
    }
    /* Never append over the tree that was just loaded. */
    uint32_t end = align4k(c->sb.root_start +
                           (c->sb.root_size + 511u) / 512u);
    for (uint32_t i = 0; i < LFC1_MAX_NODES; i++) {
        lfc1_node_t *n = &c->nodes[i];
        if (!n->used) {
            continue;
        }
        uint32_t e = align4k(n->start + (n->size + 511u) / 512u);
        if (e > end) {
            end = e;
        }
    }
    if (c->sb.next_free < end) {
        c->sb.next_free = end;
    }
    uint32_t limit = c->sb.journal_start ? c->sb.journal_start
                                         : c->sb.total_sectors;
    if (c->sb.next_free > limit) {
        node_free_tree(c, &c->nodes[0]);
        hal_pages_free(c, sizeof(*c));
        return FINTE_ERR_IO;
    }
    if (c->sb.journal_start &&
        (c->sb.free_sectors > limit - c->sb.next_free ||
         c->sb.free_sectors == 0)) {
        c->sb.free_sectors = limit - c->sb.next_free;
    }
    fs->ctx = c;
    return 0;
}

static void lfc1_umount(void *ctx)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c) {
        return;
    }
    if (c->writable) {
        (void)lfc1_sync(c);
    }
    node_free_tree(c, &c->nodes[0]);
    hal_pages_free(c, sizeof(*c));
}

static int lfc1_stat(void *ctx, const char *rel, fdfs_stat_t *out)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    lfc1_node_t *n = NULL;
    int rc = resolve(c, rel, &n);
    if (rc != 0) {
        return rc;
    }
    out->size = n->size;
    out->mode = n->is_dir ? FDISK_MODE_DIR : FDISK_MODE_FILE;
    out->reserved = 0;
    return 0;
}

static int lfc1_ls(void *ctx, const char *rel, fdfs_dir_cb cb, void *cbctx)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    lfc1_node_t *n = NULL;
    int rc = resolve(c, rel, &n);
    if (rc != 0) {
        return rc;
    }
    if (!n->is_dir) {
        return FINTE_ERR_TYPE;
    }
    for (lfc1_node_t *ch = n->child; ch; ch = ch->next) {
        fdfs_stat_t st;
        st.size = ch->size;
        st.mode = ch->is_dir ? FDISK_MODE_DIR : FDISK_MODE_FILE;
        st.reserved = 0;
        cb(cbctx, ch->name, &st);
    }
    return 0;
}

static int64_t lfc1_read(void *ctx, const char *rel, uint64_t off, void *buf,
                         size_t len)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    lfc1_node_t *n = NULL;
    int rc = resolve(c, rel, &n);
    if (rc != 0) {
        return rc;
    }
    if (n->is_dir) {
        return FINTE_ERR_TYPE;
    }
    if (off >= n->size) {
        return 0;
    }
    uint64_t avail = n->size - off;
    if (len > avail) {
        len = (size_t)avail;
    }
    finte_memcpy(buf, n->data + off, len);
    return (int64_t)len;
}

static int lfc1_info(void *ctx, fdfs_info_t *out)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    finte_strlcpy(out->fstype, "lfc1", sizeof(out->fstype));
    size_t n = 0;
    while (n < sizeof(c->sb.label) && n + 1 < sizeof(out->label) &&
           c->sb.label[n]) {
        char ch = c->sb.label[n];
        out->label[n] = (ch >= 32 && ch < 127) ? ch : '?';
        n++;
    }
    out->label[n] = 0;
    out->block_size = c->sb.block_size;
    out->total_bytes = (uint64_t)c->sb.total_sectors * 512u;
    uint32_t used = c->sb.next_free;
    if (used > c->sb.total_sectors) {
        used = c->sb.total_sectors;
    }
    out->used_bytes = (uint64_t)used * 512u;
    out->free_bytes = (uint64_t)(c->sb.total_sectors - used) * 512u;
    return 0;
}

static int lfc1_create(void *ctx, const char *rel, int is_dir)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c->writable) {
        return FINTE_ERR_PERM;
    }
    lfc1_node_t *parent = NULL;
    char name[LFC1_NAME_MAX + 1];
    int rc = resolve_parent(c, rel, &parent, name, sizeof(name));
    if (rc != 0) {
        return rc;
    }
    if (!parent->is_dir) {
        return FINTE_ERR_TYPE;
    }
    if (node_child(parent, name)) {
        return FINTE_ERR_EXIST;
    }
    lfc1_node_t *n = node_alloc(c, name, is_dir);
    if (!n) {
        return FINTE_ERR_NOMEM;
    }
    node_attach(parent, n);
    c->dirty = 1;
    return 0;
}

static int lfc1_unlink(void *ctx, const char *rel)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c->writable) {
        return FINTE_ERR_PERM;
    }
    lfc1_node_t *n = NULL;
    int rc = resolve(c, rel, &n);
    if (rc != 0) {
        return rc;
    }
    if (n == &c->nodes[0]) {
        return FINTE_ERR_PERM;
    }
    if (n->is_dir && n->child) {
        return FINTE_ERR_BUSY;
    }
    node_detach(n);
    node_free_tree(c, n);
    c->dirty = 1;
    return 0;
}

static int lfc1_rename(void *ctx, const char *from, const char *to)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c->writable) {
        return FINTE_ERR_PERM;
    }
    lfc1_node_t *n = NULL;
    int rc = resolve(c, from, &n);
    if (rc != 0) {
        return rc;
    }
    if (n == &c->nodes[0]) {
        return FINTE_ERR_PERM;
    }
    lfc1_node_t *parent = NULL;
    char name[LFC1_NAME_MAX + 1];
    rc = resolve_parent(c, to, &parent, name, sizeof(name));
    if (rc != 0) {
        return rc;
    }
    if (!parent->is_dir) {
        return FINTE_ERR_TYPE;
    }
    if (node_child(parent, name)) {
        return FINTE_ERR_EXIST;
    }
    if (n->is_dir) {
        for (lfc1_node_t *p = parent; p; p = p->parent) {
            if (p == n) {
                return FINTE_ERR_INVAL;
            }
        }
    }
    node_detach(n);
    finte_strlcpy(n->name, name, sizeof(n->name));
    node_attach(parent, n);
    c->dirty = 1;
    return 0;
}

static int64_t lfc1_write(void *ctx, const char *rel, uint64_t off,
                          const void *buf, size_t len)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c->writable) {
        return FINTE_ERR_PERM;
    }
    lfc1_node_t *n = NULL;
    int rc = resolve(c, rel, &n);
    if (rc != 0) {
        return rc;
    }
    if (n->is_dir) {
        return FINTE_ERR_TYPE;
    }
    if (off > LFC1_MAX_DATA || len > LFC1_MAX_DATA ||
        off + len > LFC1_MAX_DATA) {
        return FINTE_ERR_LIMIT;
    }
    uint64_t end = off + len;
    uint32_t new_size = n->size;
    if (end > new_size) {
        new_size = (uint32_t)end;
    }
    uint32_t alloc = (new_size + 4095u) & ~4095u;
    if (alloc == 0) {
        alloc = 4096;
    }
    uint8_t *nb = (uint8_t *)hal_pages_alloc(alloc);
    if (!nb) {
        return FINTE_ERR_NOMEM;
    }
    if (n->data && n->size) {
        finte_memcpy(nb, n->data, n->size);
    }
    if (off > n->size) {
        finte_memset(nb + n->size, 0, (size_t)off - n->size);
    }
    if (len) {
        finte_memcpy(nb + off, buf, len);
    }
    if (c->data_bytes - n->size + new_size > LFC1_MAX_DATA) {
        hal_pages_free(nb, alloc);
        return FINTE_ERR_NOMEM;
    }
    node_free_data(c, n);
    n->data = nb;
    n->size = new_size;
    c->data_bytes += new_size;
    c->dirty = 1;
    return (int64_t)len;
}
static int lfc1_truncate(void *ctx, const char *rel, uint64_t size)
{
    lfc1_ctx_t *c = (lfc1_ctx_t *)ctx;
    if (!c->writable) {
        return FINTE_ERR_PERM;
    }
    if (size > LFC1_MAX_DATA) {
        return FINTE_ERR_LIMIT;
    }
    lfc1_node_t *n = NULL;
    int rc = resolve(c, rel, &n);
    if (rc != 0) {
        return rc;
    }
    if (n->is_dir) {
        return FINTE_ERR_TYPE;
    }
    if (size == 0) {
        node_free_data(c, n);
        c->dirty = 1;
        return 0;
    }
    uint32_t alloc = ((uint32_t)size + 4095u) & ~4095u;
    uint8_t *nb = (uint8_t *)hal_pages_alloc(alloc);
    if (!nb) {
        return FINTE_ERR_NOMEM;
    }
    finte_memset(nb, 0, (size_t)size);
    if (n->data && n->size) {
        uint32_t copy = n->size < size ? n->size : (uint32_t)size;
        finte_memcpy(nb, n->data, copy);
    }
    if (c->data_bytes - n->size + size > LFC1_MAX_DATA) {
        hal_pages_free(nb, alloc);
        return FINTE_ERR_NOMEM;
    }
    node_free_data(c, n);
    n->data = nb;
    n->size = (uint32_t)size;
    c->data_bytes += (uint32_t)size;
    c->dirty = 1;
    return 0;
}

const fdfs_ops_t fdfs_lfc1 = {
    "lfc1",
    lfc1_mount,
    lfc1_umount,
    lfc1_stat,
    lfc1_ls,
    lfc1_read,
    lfc1_info,
    lfc1_create,
    lfc1_unlink,
    lfc1_rename,
    lfc1_write,
    lfc1_truncate,
    lfc1_sync,
};
