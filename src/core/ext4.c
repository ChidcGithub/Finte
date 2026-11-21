/* Read-only ext4 driver.
 *
 * Supports extent-mapped files and directories, 64-bit block numbers and
 * multi-block group descriptor tables. Directory htree blocks are read as
 * ordinary leaf blocks, so no B-tree traversal is required for listing or
 * lookup. Journaling, inline data, encryption and meta_bg are rejected. */
#include "fs_internal.h"

#define EXT4_MAGIC 0xEF53
#define EXT4_EXT_MAGIC 0xF30A
#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_INCOMPAT_COMPRESSION 0x0001u
#define EXT4_INCOMPAT_FILETYPE 0x0002u
#define EXT4_INCOMPAT_JOURNAL_DEV 0x0008u
#define EXT4_INCOMPAT_META_BG 0x0010u
#define EXT4_INCOMPAT_EXTENTS 0x0040u
#define EXT4_INCOMPAT_64BIT 0x0080u
#define EXT4_INCOMPAT_FLEX_BG 0x0200u
#define EXT4_INCOMPAT_EA_INODE 0x0400u
#define EXT4_INCOMPAT_DIRDATA 0x1000u
#define EXT4_INCOMPAT_CSUM_SEED 0x2000u
#define EXT4_INCOMPAT_LARGEDIR 0x4000u
#define EXT4_INCOMPAT_INLINE_DATA 0x8000u
#define EXT4_INCOMPAT_ENCRYPT 0x10000u
#define EXT4_INCOMPAT_CASEFOLD 0x20000u
#define EXT4_INCOMPAT_ALLOWED \
    (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | \
     EXT4_INCOMPAT_FLEX_BG | EXT4_INCOMPAT_CSUM_SEED | \
     EXT4_INCOMPAT_LARGEDIR)

#define EXT4_MAX_BLOCK 4096u
#define EXT4_MAX_INODE 1024u

typedef struct ext4_inode {
    uint32_t ino;
    uint16_t mode;
    uint16_t links;
    uint32_t flags;
    uint64_t size;
    uint8_t block[60];
} ext4_inode_t;

typedef struct ext4_ctx {
    fdfs_t *fs;
    uint32_t bs;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t groups;
    uint32_t incompat;
    uint64_t blocks_count;
    uint64_t free_blocks;
    uint64_t gdt_block;
    char label[16];
    uint8_t meta[EXT4_MAX_BLOCK];
    uint8_t data[EXT4_MAX_BLOCK];
    uint8_t inode[EXT4_MAX_INODE];
} ext4_ctx_t;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_power_of_two(uint32_t v)
{
    return v && (v & (v - 1)) == 0;
}

static int ext4_read_block(ext4_ctx_t *c, uint64_t phys, uint8_t *buf)
{
    if (phys >= c->blocks_count) {
        return FINTE_ERR_IO;
    }
    if (fdfs_read_bytes(c->fs, phys * c->bs, buf, c->bs) != 0) {
        return FINTE_ERR_IO;
    }
    return 0;
}

static int ext4_load_inode(ext4_ctx_t *c, uint32_t ino, ext4_inode_t *out)
{
    if (ino == 0 || ino > c->inodes_count) {
        return FINTE_ERR_NOENT;
    }
    uint32_t group = (ino - 1) / c->inodes_per_group;
    uint32_t index = (ino - 1) % c->inodes_per_group;
    if (group >= c->groups) {
        return FINTE_ERR_IO;
    }
    uint8_t gd[1024];
    if (c->desc_size > sizeof(gd)) {
        return FINTE_ERR_IO;
    }
    uint64_t gd_off = c->gdt_block * c->bs + (uint64_t)group * c->desc_size;
    if (fdfs_read_bytes(c->fs, gd_off, gd, c->desc_size) != 0) {
        return FINTE_ERR_IO;
    }
    uint64_t itable = rd32(gd + 8);
    if (c->desc_size >= 64) {
        itable |= (uint64_t)rd32(gd + 40) << 32;
    }
    uint64_t off = itable * c->bs + (uint64_t)index * c->inode_size;
    if (fdfs_read_bytes(c->fs, off, c->inode, c->inode_size) != 0) {
        return FINTE_ERR_IO;
    }
    const uint8_t *p = c->inode;
    out->ino = ino;
    out->mode = rd16(p + 0);
    out->links = rd16(p + 26);
    out->flags = rd32(p + 32);
    out->size = rd32(p + 4);
    if ((out->mode & 0xF000) == 0x8000) {
        out->size |= (uint64_t)rd32(p + 108) << 32;
    }
    finte_memcpy(out->block, p + 40, 60);
    return 0;
}

static int ext4_extent_lookup(ext4_ctx_t *c, const ext4_inode_t *inode,
                              uint32_t logical, uint64_t *phys, int *uninit)
{
    uint8_t root[60];
    finte_memcpy(root, inode->block, sizeof(root));
    const uint8_t *node = root;
    uint32_t node_len = sizeof(root);
    uint8_t *heap = c->meta;
    uint32_t depth = 0;

    for (;;) {
        if (node_len < 12 || rd16(node) != EXT4_EXT_MAGIC) {
            return FINTE_ERR_IO;
        }
        uint16_t entries = rd16(node + 2);
        uint16_t max = rd16(node + 4);
        depth = rd16(node + 6);
        if (depth > 8 || 12u + (uint32_t)entries * 12u > node_len ||
            12u + (uint32_t)max * 12u > node_len) {
            return FINTE_ERR_IO;
        }
        if (depth == 0) {
            for (uint16_t i = 0; i < entries; i++) {
                const uint8_t *e = node + 12 + (uint32_t)i * 12;
                uint32_t first = rd32(e);
                uint16_t raw = rd16(e + 4);
                if (raw == 0) {
                    continue;
                }
                int u = raw > 32768;
                uint32_t len = u ? (uint32_t)(raw - 32768) : raw;
                uint64_t start = rd32(e + 8) |
                                 ((uint64_t)rd16(e + 6) << 32);
                if (logical >= first && logical - first < len) {
                    *phys = start + (logical - first);
                    *uninit = u;
                    if (*phys >= c->blocks_count) {
                        return FINTE_ERR_IO;
                    }
                    return 0;
                }
            }
            return FINTE_ERR_NOENT;
        }
        const uint8_t *chosen = NULL;
        for (uint16_t i = 0; i < entries; i++) {
            const uint8_t *e = node + 12 + (uint32_t)i * 12;
            uint32_t first = rd32(e);
            if (first <= logical) {
                chosen = e;
            } else {
                break;
            }
        }
        if (!chosen) {
            return FINTE_ERR_NOENT;
        }
        uint64_t child = rd32(chosen + 4) |
                         ((uint64_t)rd16(chosen + 8) << 32);
        if (ext4_read_block(c, child, heap) != 0) {
            return FINTE_ERR_IO;
        }
        node = heap;
        node_len = c->bs;
    }
}

static int ext4_bmap(ext4_ctx_t *c, const ext4_inode_t *inode,
                     uint32_t logical, uint64_t *phys, int *uninit)
{
    *uninit = 0;
    if (inode->flags & EXT4_EXTENTS_FL) {
        return ext4_extent_lookup(c, inode, logical, phys, uninit);
    }
    const uint8_t *b = inode->block;
    if (logical < 12) {
        uint32_t p = rd32(b + logical * 4);
        if (p == 0) {
            return FINTE_ERR_NOENT;
        }
        *phys = p;
        return 0;
    }
    if (logical < 12 + c->bs / 4) {
        uint32_t ind = rd32(b + 12 * 4);
        if (ind == 0) {
            return FINTE_ERR_NOENT;
        }
        uint8_t *tmp = c->data;
        if (ext4_read_block(c, ind, tmp) != 0) {
            return FINTE_ERR_IO;
        }
        uint32_t p = rd32(tmp + (logical - 12) * 4);
        if (p == 0) {
            return FINTE_ERR_NOENT;
        }
        *phys = p;
        return 0;
    }
    return FINTE_ERR_NOENT;
}

typedef struct ext4_dirent {
    uint32_t ino;
    const char *name;
    uint8_t name_len;
} ext4_dirent_t;

/* Scan every directory data block. htree index blocks carry one fake
 * dirent with rec_len covering the rest of the block, so they are skipped
 * automatically. */
static int ext4_dir_scan(ext4_ctx_t *c, const ext4_inode_t *dir,
                         const char *want, uint32_t *ino_out,
                         fdfs_dir_cb cb, void *cbctx)
{
    if ((dir->mode & 0xF000) != 0x4000) {
        return FINTE_ERR_TYPE;
    }
    uint64_t blocks = (dir->size + c->bs - 1) / c->bs;
    if (blocks > 0x100000) {
        return FINTE_ERR_IO;
    }
    char name[256];
    for (uint64_t bi = 0; bi < blocks; bi++) {
        uint64_t phys;
        int uninit = 0;
        int rc = ext4_bmap(c, dir, (uint32_t)bi, &phys, &uninit);
        if (rc == FINTE_ERR_NOENT) {
            continue;
        }
        if (rc != 0 || uninit) {
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (ext4_read_block(c, phys, c->data) != 0) {
            return FINTE_ERR_IO;
        }
        uint32_t off = 0;
        while (off + 8 <= c->bs) {
            const uint8_t *e = c->data + off;
            uint32_t ino = rd32(e);
            uint16_t rec = rd16(e + 4);
            uint8_t nlen = e[6];
            if (rec < 8 || (rec & 3) || off + rec > c->bs) {
                break;
            }
            if (ino == 0 || nlen == 0 || (uint32_t)nlen > (uint32_t)rec - 8) {
                off += rec;
                continue;
            }
            const char *nm = (const char *)(e + 8);
            if (!(nlen == 1 && nm[0] == '.') &&
                !(nlen == 2 && nm[0] == '.' && nm[1] == '.')) {
                if (want) {
                    if (finte_strlen(want) == nlen &&
                        finte_memcmp(nm, want, nlen) == 0) {
                        *ino_out = ino;
                        return 0;
                    }
                } else if (cb) {
                    finte_memcpy(name, nm, nlen);
                    name[nlen] = 0;
                    ext4_inode_t child;
                    if (ext4_load_inode(c, ino, &child) == 0) {
                        fdfs_stat_t st;
                        st.size = child.size;
                        st.mode = (child.mode & 0xF000) == 0x4000
                                      ? FDISK_MODE_DIR : FDISK_MODE_FILE;
                        st.reserved = 0;
                        cb(cbctx, name, &st);
                    }
                }
            }
            off += rec;
        }
    }
    return want ? FINTE_ERR_NOENT : 0;
}

static int ext4_resolve(ext4_ctx_t *c, const char *rel, ext4_inode_t *out)
{
    if (ext4_load_inode(c, 2, out) != 0) {
        return FINTE_ERR_IO;
    }
    const char *p = rel;
    char name[256];
    for (;;) {
        int more = fdfs_split_next(&p, name, sizeof(name));
        if (more < 0) {
            return more;
        }
        if (more == 0) {
            return 0;
        }
        if (finte_strcmp(name, ".") == 0) {
            continue;
        }
        if (finte_strcmp(name, "..") == 0) {
            return FINTE_ERR_NOENT;
        }
        uint32_t ino = 0;
        int rc = ext4_dir_scan(c, out, name, &ino, NULL, NULL);
        if (rc != 0) {
            return rc;
        }
        rc = ext4_load_inode(c, ino, out);
        if (rc != 0) {
            return rc;
        }
    }
}

static int ext4_mount(fdfs_t *fs)
{
    uint8_t sb[1024];
    if (fdfs_read_bytes(fs, 1024, sb, sizeof(sb)) != 0) {
        return FINTE_ERR_IO;
    }
    if (rd16(sb + 56) != EXT4_MAGIC) {
        return FINTE_ERR_TYPE;
    }
    uint32_t log_bs = rd32(sb + 24);
    if (log_bs > 6) {
        return FINTE_ERR_TYPE;
    }
    uint32_t bs = 1024u << log_bs;
    if (bs < 1024 || bs > EXT4_MAX_BLOCK || !is_power_of_two(bs)) {
        return FINTE_ERR_TYPE;
    }
    uint32_t incompat = rd32(sb + 96);
    uint32_t ro_compat = rd32(sb + 100);
    if ((incompat & ~EXT4_INCOMPAT_ALLOWED) ||
        (ro_compat & 0x00000200u)) { /* unsupported bigalloc */
        return FINTE_ERR_TYPE;
    }
    uint32_t rev = rd32(sb + 76);
    uint32_t inode_size = rev == 0 ? 128 : rd16(sb + 88);
    if (inode_size < 128 || inode_size > EXT4_MAX_INODE ||
        !is_power_of_two(inode_size)) {
        return FINTE_ERR_TYPE;
    }
    uint64_t blocks = rd32(sb + 4);
    if (incompat & EXT4_INCOMPAT_64BIT) {
        blocks |= (uint64_t)rd32(sb + 336) << 32;
    }
    uint32_t bpg = rd32(sb + 32);
    uint32_t ipg = rd32(sb + 40);
    uint32_t inodes = rd32(sb + 0);
    if (blocks == 0 || bpg == 0 || ipg == 0 || inodes == 0) {
        return FINTE_ERR_IO;
    }
    uint32_t desc_size = 32;
    if (incompat & EXT4_INCOMPAT_64BIT) {
        desc_size = rd16(sb + 254);
        if (desc_size == 0) {
            desc_size = 64;
        }
        if (desc_size < 32 || desc_size > 1024 || !is_power_of_two(desc_size)) {
            return FINTE_ERR_TYPE;
        }
    }
    uint64_t groups64 = (blocks + bpg - 1) / bpg;
    uint64_t igroups = (inodes + ipg - 1) / ipg;
    if (igroups > groups64) {
        groups64 = igroups;
    }
    if (groups64 == 0 || groups64 > 0x10000) {
        return FINTE_ERR_TYPE;
    }

    ext4_ctx_t *c = (ext4_ctx_t *)hal_pages_alloc(sizeof(*c));
    if (!c) {
        return FINTE_ERR_NOMEM;
    }
    finte_memset(c, 0, sizeof(*c));
    c->fs = fs;
    c->bs = bs;
    c->inodes_count = inodes;
    c->inodes_per_group = ipg;
    c->blocks_per_group = bpg;
    c->inode_size = inode_size;
    c->desc_size = desc_size;
    c->groups = (uint32_t)groups64;
    c->incompat = incompat;
    c->blocks_count = blocks;
    c->gdt_block = (bs == 1024) ? 2 : 1;
    uint64_t free_blocks = rd32(sb + 12);
    if (incompat & EXT4_INCOMPAT_64BIT) {
        free_blocks |= (uint64_t)rd32(sb + 344) << 32;
    }
    c->free_blocks = free_blocks;
    size_t ln = 0;
    while (ln < sizeof(c->label) && ln < 16 && sb[120 + ln]) {
        char ch = (char)sb[120 + ln];
        c->label[ln] = (ch >= 32 && ch < 127) ? ch : '?';
        ln++;
    }
    c->label[ln] = 0;

    ext4_inode_t root;
    if (ext4_load_inode(c, 2, &root) != 0 ||
        (root.mode & 0xF000) != 0x4000) {
        hal_pages_free(c, sizeof(*c));
        return FINTE_ERR_IO;
    }
    fs->ctx = c;
    return 0;
}

static void ext4_umount(void *ctx)
{
    if (ctx) {
        hal_pages_free(ctx, sizeof(ext4_ctx_t));
    }
}

static int ext4_stat(void *ctx, const char *rel, fdfs_stat_t *out)
{
    ext4_ctx_t *c = (ext4_ctx_t *)ctx;
    ext4_inode_t ino;
    int rc = ext4_resolve(c, rel, &ino);
    if (rc != 0) {
        return rc;
    }
    out->size = ino.size;
    out->mode = (ino.mode & 0xF000) == 0x4000 ? FDISK_MODE_DIR
                                                : FDISK_MODE_FILE;
    out->reserved = 0;
    return 0;
}

static int ext4_ls(void *ctx, const char *rel, fdfs_dir_cb cb, void *cbctx)
{
    ext4_ctx_t *c = (ext4_ctx_t *)ctx;
    ext4_inode_t ino;
    int rc = ext4_resolve(c, rel, &ino);
    if (rc != 0) {
        return rc;
    }
    return ext4_dir_scan(c, &ino, NULL, NULL, cb, cbctx);
}

static int64_t ext4_read(void *ctx, const char *rel, uint64_t off, void *buf,
                         size_t len)
{
    ext4_ctx_t *c = (ext4_ctx_t *)ctx;
    ext4_inode_t ino;
    int rc = ext4_resolve(c, rel, &ino);
    if (rc != 0) {
        return rc;
    }
    if ((ino.mode & 0xF000) != 0x8000) {
        return FINTE_ERR_TYPE;
    }
    if (off >= ino.size) {
        return 0;
    }
    uint64_t avail = ino.size - off;
    if (len > avail) {
        len = (size_t)avail;
    }
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t abs = off + done;
        uint32_t logical = (uint32_t)(abs / c->bs);
        uint32_t boff = (uint32_t)(abs % c->bs);
        uint32_t chunk = c->bs - boff;
        if (chunk > len - done) {
            chunk = (uint32_t)(len - done);
        }
        uint64_t phys = 0;
        int uninit = 0;
        rc = ext4_bmap(c, &ino, logical, &phys, &uninit);
        if (rc == FINTE_ERR_NOENT || uninit) {
            finte_memset(out + done, 0, chunk);
        } else if (rc != 0) {
            return rc;
        } else if (boff == 0 && chunk == c->bs) {
            if (fdfs_read_bytes(c->fs, phys * c->bs, out + done, chunk) != 0) {
                return FINTE_ERR_IO;
            }
        } else {
            if (ext4_read_block(c, phys, c->data) != 0) {
                return FINTE_ERR_IO;
            }
            finte_memcpy(out + done, c->data + boff, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int ext4_info(void *ctx, fdfs_info_t *out)
{
    ext4_ctx_t *c = (ext4_ctx_t *)ctx;
    finte_strlcpy(out->fstype, "ext4", sizeof(out->fstype));
    finte_strlcpy(out->label, c->label, sizeof(out->label));
    out->block_size = c->bs;
    out->total_bytes = c->blocks_count * c->bs;
    uint64_t used = 0;
    if (c->free_blocks < c->blocks_count) {
        used = (c->blocks_count - c->free_blocks) * c->bs;
    }
    out->used_bytes = used;
    out->free_bytes = c->free_blocks * c->bs;
    return 0;
}

const fdfs_ops_t fdfs_ext4 = {
    "ext4",
    ext4_mount,
    ext4_umount,
    ext4_stat,
    ext4_ls,
    ext4_read,
    ext4_info,
    NULL, NULL, NULL, NULL, NULL, NULL,
};
