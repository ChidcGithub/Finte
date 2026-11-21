/* Read-only NTFS driver.
 *
 * Reads the boot sector, $MFT runlist, FILE records with update-sequence
 * fixups, resident/non-resident $DATA streams, and $I30 directory indexes.
 * Directory indexes are scanned as root plus allocation blocks; entries are
 * deduplicated by MFT reference. Compression, encryption and attribute lists
 * are rejected. */
#include "fs_internal.h"

#define NTFS_MAX_RUNS 32
#define NTFS_MAX_RECORD 4096
#define NTFS_MAX_INDEX 4096
#define NTFS_MAX_NAME 255
#define NTFS_MFT_ROOT 5

#define NTFS_ATTR_STANDARD 0x10
#define NTFS_ATTR_LIST 0x20
#define NTFS_ATTR_FILE_NAME 0x30
#define NTFS_ATTR_VOLUME_NAME 0x60
#define NTFS_ATTR_DATA 0x80
#define NTFS_ATTR_INDEX_ROOT 0x90
#define NTFS_ATTR_INDEX_ALLOC 0xA0
#define NTFS_ATTR_BITMAP 0xB0
#define NTFS_ATTR_END 0xFFFFFFFFu
#define NTFS_ATTR_FLAG_COMPRESSED 0x0001
#define NTFS_ATTR_FLAG_ENCRYPTED 0x4000

typedef struct ntfs_run {
    uint64_t vcn;
    uint64_t lcn;
    uint32_t clusters;
    int sparse;
} ntfs_run_t;

typedef struct ntfs_stream {
    ntfs_run_t runs[NTFS_MAX_RUNS];
    int count;
    uint64_t size;
} ntfs_stream_t;

typedef struct ntfs_file {
    uint64_t recno;
    int is_dir;
    int has_data;
    int data_resident;
    uint16_t data_flags;
    uint64_t data_size;
    ntfs_stream_t data;
    uint32_t res_len;
    int has_index;
    uint16_t index_flags;
    uint32_t index_block_size;
    int has_index_alloc;
    ntfs_stream_t index_alloc;
    uint32_t index_root_len;
    uint8_t *index_root;
    const uint8_t *res_data;
} ntfs_file_t;

typedef struct ntfs_ctx {
    fdfs_t *fs;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t record_size;
    uint32_t index_size;
    uint64_t total_clusters;
    uint64_t mft_lcn;
    uint64_t mft_count;
    ntfs_stream_t mft;
    char label[FDISK_LABEL_MAX];
    uint8_t rec[NTFS_MAX_RECORD];
    uint8_t idx[NTFS_MAX_INDEX];
    uint8_t file_root[NTFS_MAX_INDEX];
    uint8_t file_res[1024];
} ntfs_ctx_t;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int decode_signed8(uint8_t v)
{
    return (int)(int8_t)v;
}

static int ntfs_fixup(uint8_t *rec, uint32_t size, const char *magic)
{
    if (finte_memcmp(rec, magic, 4) != 0) {
        return -1;
    }
    uint16_t usa_off = rd16(rec + 4);
    uint16_t usa_count = rd16(rec + 6);
    if (usa_count != size / 512 + 1 ||
        (uint32_t)usa_off + (uint32_t)usa_count * 2u > size) {
        return -1;
    }
    uint16_t usn = rd16(rec + usa_off);
    for (uint16_t i = 1; i < usa_count; i++) {
        uint32_t at = i * 512 - 2;
        if (rd16(rec + at) != usn) {
            return -1;
        }
        wr16(rec + at, rd16(rec + usa_off + i * 2));
    }
    return 0;
}

static int ntfs_mount_size(uint8_t v, uint32_t cluster, uint32_t max,
                           uint32_t *out)
{
    int n = decode_signed8(v);
    uint64_t size;
    if (n < 0) {
        if (n < -16) {
            return -1;
        }
        size = 1ull << (unsigned)(-n);
    } else {
        size = (uint64_t)n * cluster;
    }
    if (size < 512 || size > max || (size & (size - 1)) != 0) {
        return -1;
    }
    *out = (uint32_t)size;
    return 0;
}

static int ntfs_stream_read(ntfs_ctx_t *c, const ntfs_stream_t *s,
                            uint64_t off, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint64_t cluster = off / c->cluster_size;
        uint32_t in = (uint32_t)(off % c->cluster_size);
        const ntfs_run_t *run = NULL;
        for (int i = 0; i < s->count; i++) {
            if (cluster >= s->runs[i].vcn &&
                cluster < s->runs[i].vcn + s->runs[i].clusters) {
                run = &s->runs[i];
                break;
            }
        }
        uint32_t chunk = c->cluster_size - in;
        if (chunk > len) {
            chunk = (uint32_t)len;
        }
        if (!run) {
            finte_memset(out, 0, chunk);
        } else {
            uint64_t avail = (run->vcn + run->clusters - cluster) *
                             c->cluster_size - in;
            if (chunk > avail) {
                chunk = (uint32_t)avail;
            }
            if (run->sparse) {
                finte_memset(out, 0, chunk);
            } else {
                uint64_t phys = (run->lcn + (cluster - run->vcn)) *
                                c->cluster_size + in;
                if (fdfs_read_bytes(c->fs, phys, out, chunk) != 0) {
                    return FINTE_ERR_IO;
                }
            }
        }
        off += chunk;
        out += chunk;
        len -= chunk;
    }
    return 0;
}

static int ntfs_parse_runlist(ntfs_ctx_t *c, const uint8_t *p, uint32_t len,
                              ntfs_stream_t *out)
{
    int64_t lcn = 0;
    uint64_t vcn = 0;
    uint32_t at = 0;
    out->count = 0;
    while (at < len) {
        uint8_t head = p[at++];
        if (head == 0) {
            return 0;
        }
        uint32_t lsize = head & 0x0F;
        uint32_t osize = head >> 4;
        if (lsize == 0 || lsize > 8 || osize > 8 || at + lsize + osize > len) {
            return FINTE_ERR_IO;
        }
        uint64_t run_len = 0;
        for (uint32_t i = 0; i < lsize; i++) {
            run_len |= (uint64_t)p[at + i] << (8 * i);
        }
        at += lsize;
        int64_t delta = 0;
        if (osize) {
            uint64_t raw = 0;
            for (uint32_t i = 0; i < osize; i++) {
                raw |= (uint64_t)p[at + i] << (8 * i);
            }
            if (osize < 8 && (p[at + osize - 1] & 0x80)) {
                raw |= ~0ull << (8 * osize);
            }
            delta = (int64_t)raw;
        }
        at += osize;
        if (run_len == 0 || out->count >= NTFS_MAX_RUNS) {
            return FINTE_ERR_IO;
        }
        lcn += delta;
        ntfs_run_t *r = &out->runs[out->count++];
        r->vcn = vcn;
        r->lcn = (uint64_t)lcn;
        r->clusters = (uint32_t)run_len;
        r->sparse = (osize == 0);
        if (!r->sparse &&
            ((lcn < 0) || (uint64_t)lcn + run_len > c->total_clusters)) {
            return FINTE_ERR_IO;
        }
        vcn += run_len;
    }
    return FINTE_ERR_IO;
}

static int ntfs_parse_attrs(ntfs_ctx_t *c, uint8_t *rec, ntfs_file_t *out)
{
    finte_memset(out, 0, sizeof(*out));
    out->recno = 0;
    uint16_t attr_off = rd16(rec + 0x14);
    uint16_t flags = rd16(rec + 0x16);
    uint32_t used = rd32(rec + 0x18);
    if (used < 0x18 || used > c->record_size ||
        attr_off < 0x18 || attr_off > used) {
        return FINTE_ERR_IO;
    }
    out->is_dir = (flags & 0x2) != 0;
    out->index_root = c->file_root;

    uint32_t off = attr_off;
    while (off + 16 <= used) {
        const uint8_t *a = rec + off;
        uint32_t type = rd32(a);
        if (type == NTFS_ATTR_END) {
            break;
        }
        uint32_t alen = rd32(a + 4);
        if (alen < 16 || alen > used - off) {
            return FINTE_ERR_IO;
        }
        uint8_t nonres = a[8];
        uint8_t name_len = a[9];
        uint16_t aflags = rd16(a + 12);

        if (type == NTFS_ATTR_LIST) {
            return FINTE_ERR_TYPE;
        }
        if (type == NTFS_ATTR_DATA && name_len == 0) {
            out->has_data = 1;
            out->data_flags = aflags;
            if (nonres) {
                uint64_t dsize = rd64(a + 0x30);
                uint16_t roff = rd16(a + 0x20);
                if (roff >= alen ||
                    ntfs_parse_runlist(c, a + roff, alen - roff,
                                       &out->data) != 0) {
                    return FINTE_ERR_IO;
                }
                out->data.size = dsize;
                out->data_size = dsize;
            } else {
                uint32_t vlen = rd32(a + 0x10);
                uint16_t voff = rd16(a + 0x14);
                if (voff + vlen > alen || vlen > sizeof(c->file_res)) {
                    return FINTE_ERR_IO;
                }
                finte_memcpy(c->file_res, a + voff, vlen);
                out->data_resident = 1;
                out->res_data = c->file_res;
                out->res_len = vlen;
                out->data_size = vlen;
            }
        } else if (type == NTFS_ATTR_INDEX_ROOT && !nonres) {
            uint32_t vlen = rd32(a + 0x10);
            uint16_t voff = rd16(a + 0x14);
            if (voff + vlen > alen || vlen > sizeof(c->file_root) || vlen < 32) {
                return FINTE_ERR_IO;
            }
            finte_memcpy(c->file_root, a + voff, vlen);
            out->has_index = 1;
            out->index_flags = aflags;
            out->index_root_len = vlen;
            out->index_block_size = rd32(c->file_root + 8);
        } else if (type == NTFS_ATTR_INDEX_ALLOC) {
            uint64_t dsize = rd64(a + 0x30);
            uint16_t roff = rd16(a + 0x20);
            if (nonres) {
                if (roff >= alen ||
                    ntfs_parse_runlist(c, a + roff, alen - roff,
                                       &out->index_alloc) != 0) {
                    return FINTE_ERR_IO;
                }
                out->index_alloc.size = dsize;
                out->has_index_alloc = 1;
            }
        } else if (type == NTFS_ATTR_VOLUME_NAME && !nonres) {
            uint32_t vlen = rd32(a + 0x10);
            uint16_t voff = rd16(a + 0x14);
            if (voff + vlen <= alen) {
                size_t n = 0;
                for (uint32_t i = 0; i + 1 < vlen && n + 1 < sizeof(c->label);
                     i += 2) {
                    uint16_t ch = rd16(a + voff + i);
                    if (ch == 0) {
                        break;
                    }
                    c->label[n++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
                }
                c->label[n] = 0;
            }
        }
        off += alen;
    }
    if (!out->has_data && !out->is_dir) {
        /* Files with no unnamed data are still valid (empty file). */
        out->data_size = 0;
    }
    return 0;
}

static int ntfs_read_record(ntfs_ctx_t *c, uint64_t recno, ntfs_file_t *out)
{
    if (recno >= c->mft_count) {
        return FINTE_ERR_NOENT;
    }
    uint64_t off = recno * c->record_size;
    if (ntfs_stream_read(c, &c->mft, off, c->rec, c->record_size) != 0) {
        return FINTE_ERR_IO;
    }
    if (ntfs_fixup(c->rec, c->record_size, "FILE") != 0) {
        return FINTE_ERR_IO;
    }
    uint16_t flags = rd16(c->rec + 0x16);
    if (!(flags & 0x1)) {
        return FINTE_ERR_NOENT;
    }
    int rc = ntfs_parse_attrs(c, c->rec, out);
    if (rc == 0) {
        out->recno = recno;
    }
    return rc;
}

static char ntfs_fold(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int ntfs_name_eq_ascii(const char *a, const char *b)
{
    while (*a && *b) {
        if (ntfs_fold(*a) != ntfs_fold(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

typedef struct ntfs_ent {
    uint64_t ref;
    uint64_t real_size;
    uint32_t file_attr;
    int is_dir;
    int last;
    int has_sub;
    uint8_t name_len;
    char name[NTFS_MAX_NAME + 1];
} ntfs_ent_t;

static int ntfs_entry_parse(const uint8_t *e, uint32_t avail, ntfs_ent_t *out)
{
    if (avail < 16) {
        return -1;
    }
    uint16_t elen = rd16(e + 8);
    uint16_t klen = rd16(e + 10);
    uint16_t flags = rd16(e + 12);
    if (elen < 16 || elen > avail) {
        return -1;
    }
    finte_memset(out, 0, sizeof(*out));
    out->ref = rd64(e) & 0x0000FFFFFFFFFFFFull;
    out->last = (flags & 0x2) != 0;
    out->has_sub = (flags & 0x1) != 0;
    if (out->last || klen < 66 || (uint32_t)klen + 16 > elen) {
        return 0;
    }
    const uint8_t *k = e + 16;
    out->real_size = rd64(k + 48);
    out->file_attr = rd32(k + 56);
    out->is_dir = (out->file_attr & 0x10000000u) != 0;
    uint8_t nl = k[64];
    if (66u + (uint32_t)nl * 2u > klen) {
        return -1;
    }
    size_t o = 0;
    for (uint8_t i = 0; i < nl; i++) {
        uint16_t ch = rd16(k + 66 + i * 2);
        out->name[o++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    out->name[o] = 0;
    out->name_len = (uint8_t)o;
    return 0;
}

typedef struct ntfs_seen {
    uint64_t refs[128];
    int count;
} ntfs_seen_t;

static int ntfs_scan_entries(const uint8_t *base, uint32_t avail,
                             const char *want, uint64_t *ref_out,
                             fdfs_dir_cb cb, void *cbctx, ntfs_seen_t *seen)
{
    uint32_t off = 0;
    while (off + 16 <= avail) {
        const uint8_t *e = base + off;
        ntfs_ent_t ent;
        if (ntfs_entry_parse(e, avail - off, &ent) != 0) {
            break;
        }
        uint16_t elen = rd16(e + 8);
        if (ent.last) {
            return want ? FINTE_ERR_NOENT : 0;
        }
        if (ent.name_len && ent.ref) {
            if (ent.name_len == 1 && ent.name[0] == '.') {
                off += elen;
                continue;
            }
            if (ent.name_len == 2 && ent.name[0] == '.' && ent.name[1] == '.') {
                off += elen;
                continue;
            }
            if (want) {
                if (ntfs_name_eq_ascii(ent.name, want)) {
                    *ref_out = ent.ref;
                    return 0;
                }
            } else if (cb) {
                int dup = 0;
                for (int i = 0; i < seen->count; i++) {
                    if (seen->refs[i] == ent.ref) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup && seen->count < 128) {
                    seen->refs[seen->count++] = ent.ref;
                    fdfs_stat_t st;
                    st.size = ent.real_size;
                    st.mode = ent.is_dir ? FDISK_MODE_DIR : FDISK_MODE_FILE;
                    st.reserved = 0;
                    cb(cbctx, ent.name, &st);
                }
            }
        }
        off += elen;
    }
    return want ? FINTE_ERR_NOENT : 0;
}

static int ntfs_dir_scan(ntfs_ctx_t *c, ntfs_file_t *dir, const char *want,
                         uint64_t *ref_out, fdfs_dir_cb cb, void *cbctx,
                         ntfs_seen_t *seen)
{
    if (!dir->is_dir || !dir->has_index) {
        return FINTE_ERR_TYPE;
    }
    const uint8_t *v = dir->index_root;
    uint32_t vlen = dir->index_root_len;
    if (vlen < 32 || dir->index_block_size < 512 ||
        dir->index_block_size > NTFS_MAX_INDEX) {
        return FINTE_ERR_IO;
    }
    uint32_t ibs = dir->index_block_size;
    uint32_t eoff = rd32(v + 16);
    uint32_t esize = rd32(v + 20);
    if (16u + eoff > vlen) {
        return FINTE_ERR_IO;
    }
    uint32_t avail = vlen - (16u + eoff);
    if (esize < avail) {
        avail = esize;
    }
    int rc = ntfs_scan_entries(v + 16 + eoff, avail, want, ref_out,
                               cb, cbctx, seen);
    if (want && rc == 0) {
        return 0;
    }
    if (rc != 0 && rc != FINTE_ERR_NOENT) {
        return rc;
    }

    if (dir->has_index_alloc && dir->index_alloc.size) {
        uint64_t blocks = (dir->index_alloc.size + ibs - 1) / ibs;
        if (blocks > 32768) {
            return FINTE_ERR_IO;
        }
        for (uint64_t b = 0; b < blocks; b++) {
            if (ntfs_stream_read(c, &dir->index_alloc, b * ibs,
                                 c->idx, ibs) != 0) {
                return FINTE_ERR_IO;
            }
            if (finte_memcmp(c->idx, "INDX", 4) != 0) {
                continue;
            }
            if (ntfs_fixup(c->idx, ibs, "INDX") != 0) {
                continue;
            }
            uint32_t boff = rd32(c->idx + 24);
            uint32_t bsize = rd32(c->idx + 28);
            if (24u + boff > ibs) {
                continue;
            }
            uint32_t bavail = ibs - (24u + boff);
            if (bsize < bavail) {
                bavail = bsize;
            }
            rc = ntfs_scan_entries(c->idx + 24 + boff, bavail, want,
                                   ref_out, cb, cbctx, seen);
            if (want && rc == 0) {
                return 0;
            }
            if (rc != 0 && rc != FINTE_ERR_NOENT) {
                return rc;
            }
        }
    }
    return want ? FINTE_ERR_NOENT : 0;
}

static int ntfs_lookup(ntfs_ctx_t *c, uint64_t dir_rec, const char *name,
                       uint64_t *ref)
{
    ntfs_file_t dir;
    int rc = ntfs_read_record(c, dir_rec, &dir);
    if (rc != 0) {
        return rc;
    }
    return ntfs_dir_scan(c, &dir, name, ref, NULL, NULL, NULL);
}

static int ntfs_resolve(ntfs_ctx_t *c, const char *rel, ntfs_file_t *out)
{
    uint64_t cur = NTFS_MFT_ROOT;
    const char *p = rel;
    char name[256];
    for (;;) {
        int more = fdfs_split_next(&p, name, sizeof(name));
        if (more < 0) {
            return more;
        }
        if (more == 0) {
            break;
        }
        if (finte_strcmp(name, ".") == 0) {
            continue;
        }
        if (finte_strcmp(name, "..") == 0) {
            return FINTE_ERR_NOENT;
        }
        uint64_t ref = 0;
        int rc = ntfs_lookup(c, cur, name, &ref);
        if (rc != 0) {
            return rc;
        }
        cur = ref;
    }
    return ntfs_read_record(c, cur, out);
}

static int ntfs_mount(fdfs_t *fs)
{
    uint8_t boot[512];
    if (fdfs_read_sectors(fs, 0, boot, 1) != 0) {
        return FINTE_ERR_IO;
    }
    if (finte_memcmp(boot + 3, "NTFS    ", 8) != 0) {
        return FINTE_ERR_TYPE;
    }
    uint16_t bps = rd16(boot + 0x0B);
    uint8_t spc = boot[0x0D];
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) ||
        spc == 0 || (spc & (spc - 1)) || (uint32_t)bps * spc > 65536) {
        return FINTE_ERR_TYPE;
    }
    uint32_t cluster = (uint32_t)bps * spc;
    uint64_t total_sectors = rd64(boot + 0x28);
    uint64_t mft_lcn = rd64(boot + 0x30);
    uint64_t total_clusters = total_sectors / spc;
    uint32_t record_size, index_size;
    if (total_clusters == 0 || mft_lcn >= total_clusters ||
        ntfs_mount_size(boot[0x40], cluster, NTFS_MAX_RECORD,
                        &record_size) != 0 ||
        ntfs_mount_size(boot[0x44], cluster, NTFS_MAX_INDEX,
                        &index_size) != 0) {
        return FINTE_ERR_TYPE;
    }

    ntfs_ctx_t *c = (ntfs_ctx_t *)hal_pages_alloc(sizeof(*c));
    if (!c) {
        return FINTE_ERR_NOMEM;
    }
    finte_memset(c, 0, sizeof(*c));
    c->fs = fs;
    c->sector_size = bps;
    c->cluster_size = cluster;
    c->record_size = record_size;
    c->index_size = index_size;
    c->total_clusters = total_clusters;
    c->mft_lcn = mft_lcn;
    finte_strlcpy(c->label, "NTFS", sizeof(c->label));

    /* Record 0 is at the MFT start and gives us the MFT data runlist. */
    uint64_t mft_off = mft_lcn * cluster;
    if (fdfs_read_bytes(fs, mft_off, c->rec, record_size) != 0 ||
        ntfs_fixup(c->rec, record_size, "FILE") != 0) {
        hal_pages_free(c, sizeof(*c));
        return FINTE_ERR_IO;
    }
    ntfs_file_t f;
    if (ntfs_parse_attrs(c, c->rec, &f) != 0 || !f.has_data ||
        f.data_resident || f.data_flags & (NTFS_ATTR_FLAG_COMPRESSED |
                                           NTFS_ATTR_FLAG_ENCRYPTED)) {
        hal_pages_free(c, sizeof(*c));
        return FINTE_ERR_IO;
    }
    c->mft = f.data;
    c->mft_count = f.data_size / record_size;
    if (c->mft_count < 16) {
        hal_pages_free(c, sizeof(*c));
        return FINTE_ERR_IO;
    }

    ntfs_file_t root;
    if (ntfs_read_record(c, NTFS_MFT_ROOT, &root) != 0 || !root.is_dir ||
        !root.has_index) {
        hal_pages_free(c, sizeof(*c));
        return FINTE_ERR_IO;
    }
    ntfs_file_t vol;
    if (ntfs_read_record(c, 3, &vol) == 0) {
        /* ntfs_parse_attrs already copied the volume name. */
    }
    fs->ctx = c;
    return 0;
}

static void ntfs_umount(void *ctx)
{
    if (ctx) {
        hal_pages_free(ctx, sizeof(ntfs_ctx_t));
    }
}

static int ntfs_stat(void *ctx, const char *rel, fdfs_stat_t *out)
{
    ntfs_ctx_t *c = (ntfs_ctx_t *)ctx;
    ntfs_file_t f;
    int rc = ntfs_resolve(c, rel, &f);
    if (rc != 0) {
        return rc;
    }
    out->size = f.is_dir ? 0 : f.data_size;
    out->mode = f.is_dir ? FDISK_MODE_DIR : FDISK_MODE_FILE;
    out->reserved = 0;
    return 0;
}

static int ntfs_ls(void *ctx, const char *rel, fdfs_dir_cb cb, void *cbctx)
{
    ntfs_ctx_t *c = (ntfs_ctx_t *)ctx;
    ntfs_file_t f;
    int rc = ntfs_resolve(c, rel, &f);
    if (rc != 0) {
        return rc;
    }
    if (!f.is_dir) {
        return FINTE_ERR_TYPE;
    }
    ntfs_seen_t seen;
    finte_memset(&seen, 0, sizeof(seen));
    return ntfs_dir_scan(c, &f, NULL, NULL, cb, cbctx, &seen);
}

static int64_t ntfs_read(void *ctx, const char *rel, uint64_t off, void *buf,
                         size_t len)
{
    ntfs_ctx_t *c = (ntfs_ctx_t *)ctx;
    ntfs_file_t f;
    int rc = ntfs_resolve(c, rel, &f);
    if (rc != 0) {
        return rc;
    }
    if (f.is_dir) {
        return FINTE_ERR_TYPE;
    }
    if (!f.has_data || off >= f.data_size) {
        return 0;
    }
    uint64_t avail = f.data_size - off;
    if (len > avail) {
        len = (size_t)avail;
    }
    if (f.data_flags & (NTFS_ATTR_FLAG_COMPRESSED | NTFS_ATTR_FLAG_ENCRYPTED)) {
        return FINTE_ERR_TYPE;
    }
    if (f.data_resident) {
        if (off + len > f.res_len) {
            return FINTE_ERR_IO;
        }
        finte_memcpy(buf, f.res_data + off, len);
    } else if (ntfs_stream_read(c, &f.data, off, buf, len) != 0) {
        return FINTE_ERR_IO;
    }
    return (int64_t)len;
}

static int ntfs_info(void *ctx, fdfs_info_t *out)
{
    ntfs_ctx_t *c = (ntfs_ctx_t *)ctx;
    finte_strlcpy(out->fstype, "ntfs", sizeof(out->fstype));
    finte_strlcpy(out->label, c->label, sizeof(out->label));
    out->block_size = c->cluster_size;
    out->total_bytes = c->total_clusters * c->cluster_size;
    out->used_bytes = 0;
    return 0;
}

const fdfs_ops_t fdfs_ntfs = {
    "ntfs",
    ntfs_mount,
    ntfs_umount,
    ntfs_stat,
    ntfs_ls,
    ntfs_read,
    ntfs_info,
    NULL, NULL, NULL, NULL, NULL, NULL,
};
