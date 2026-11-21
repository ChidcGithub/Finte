/* Disk manager core: enumerate block devices, parse partitions, mount
 * read-only filesystems and resolve paths. */
#include "finte/disk.h"
#include "fs_internal.h"

#define SECTOR_BOUNCE 4096

static fdisk_dev_t g_disks[FDISK_MAX];
static int g_disk_count;
static int g_scanned;

typedef struct fmount {
    int used;
    fdfs_t fs;
    char source[FDISK_NAME_MAX];
    char target[FDISK_PATH_MAX];
} fmount_t;

static fmount_t g_mounts[FMOUNT_MAX];
static uint8_t g_bounce[SECTOR_BOUNCE];

#define FCACHE_ENTRIES 32
typedef struct fcache_ent {
    int valid;
    int disk;
    uint64_t start;
    uint64_t lba;
    uint64_t stamp;
    uint8_t data[4096];
} fcache_ent_t;

static fcache_ent_t g_cache[FCACHE_ENTRIES];
static uint64_t g_cache_tick;

static fcache_ent_t *cache_lookup(int disk, uint64_t start, uint64_t lba)
{
    for (int i = 0; i < FCACHE_ENTRIES; i++) {
        fcache_ent_t *e = &g_cache[i];
        if (e->valid && e->disk == disk && e->start == start &&
            e->lba == lba) {
            e->stamp = ++g_cache_tick;
            return e;
        }
    }
    return NULL;
}

static fcache_ent_t *cache_slot(void)
{
    fcache_ent_t *victim = &g_cache[0];
    for (int i = 0; i < FCACHE_ENTRIES; i++) {
        if (!g_cache[i].valid) {
            return &g_cache[i];
        }
        if (g_cache[i].stamp < victim->stamp) {
            victim = &g_cache[i];
        }
    }
    return victim;
}

static void cache_invalidate(fdfs_t *fs)
{
    for (int i = 0; i < FCACHE_ENTRIES; i++) {
        fcache_ent_t *e = &g_cache[i];
        if (e->valid && e->disk == fs->disk && e->start == fs->start) {
            e->valid = 0;
        }
    }
}

static const fdfs_ops_t *const g_fs[] = {
    &fdfs_lfc1,
    &fdfs_ext4,
    &fdfs_ntfs,
};

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

static int disk_read_sector(const fdisk_dev_t *d, uint64_t lba, void *buf)
{
    return hal_blk_read(d->hal_index, lba, buf, 1);
}

static void part_clear(fdisk_part_t *p)
{
    finte_memset(p, 0, sizeof(*p));
}

static void part_name_mbr(uint8_t type, char *out, size_t cap)
{
    static const struct {
        uint8_t type;
        const char *name;
    } names[] = {
        { 0x01, "fat12" },
        { 0x04, "fat16" },
        { 0x05, "extended" },
        { 0x06, "fat16b" },
        { 0x07, "ntfs/exfat" },
        { 0x0B, "fat32" },
        { 0x0C, "fat32 lba" },
        { 0x0E, "fat16 lba" },
        { 0x0F, "extended" },
        { 0x4C, "lfc1" },
        { 0x82, "linux swap" },
        { 0x83, "linux" },
        { 0x85, "linux ext" },
        { 0x8E, "linux lvm" },
        { 0xEE, "gpt protective" },
        { 0xEF, "efi system" },
    };
    const char *name = "unknown";
    for (size_t i = 0; i < FINTE_ARRAY_SIZE(names); i++) {
        if (names[i].type == type) {
            name = names[i].name;
            break;
        }
    }
    finte_strlcpy(out, name, cap);
}

static int guid_eq(const uint8_t *a, const uint8_t *b)
{
    return finte_memcmp(a, b, 16) == 0;
}

static void guid_name(const uint8_t *g, char *out, size_t cap)
{
    static const uint8_t efi[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0xF8, 0x11, 0xD2, 0xBA,
        0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    static const uint8_t msdata[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    static const uint8_t linuxfs[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };
    static const uint8_t lfc1[16] = {
        0x31, 0x43, 0x46, 0x4C, 0x01, 0x00, 0x46, 0x4C,
        0x80, 0x00, 0x46, 0x69, 0x6E, 0x74, 0x65, 0x00
    };
    if (guid_eq(g, efi)) {
        finte_strlcpy(out, "efi system", cap);
    } else if (guid_eq(g, msdata)) {
        finte_strlcpy(out, "microsoft data", cap);
    } else if (guid_eq(g, linuxfs)) {
        finte_strlcpy(out, "linux filesystem", cap);
    } else if (guid_eq(g, lfc1)) {
        finte_strlcpy(out, "lfc1", cap);
    } else {
        finte_strlcpy(out, "gpt", cap);
    }
}

static void gpt_label(const uint8_t *p, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < 36 && o + 1 < cap; i++) {
        uint16_t c = rd16(p + i * 2);
        if (c == 0) {
            break;
        }
        out[o++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[o] = 0;
    while (o && out[o - 1] == ' ') {
        out[--o] = 0;
    }
}

static int add_part(fdisk_dev_t *d, uint8_t number, uint8_t scheme,
                    uint8_t mbr_type, uint64_t start, uint64_t sectors,
                    const char *label, const char *type_name)
{
    if (d->part_count >= FPART_MAX || sectors == 0 || start >= d->sectors) {
        return -1;
    }
    if (start + sectors > d->sectors) {
        sectors = d->sectors - start;
    }
    fdisk_part_t *p = &d->parts[d->part_count++];
    part_clear(p);
    p->present = 1;
    p->number = number;
    p->scheme = scheme;
    p->mbr_type = mbr_type;
    p->start = start;
    p->sectors = sectors;
    if (label) {
        finte_strlcpy(p->label, label, sizeof(p->label));
    }
    if (type_name) {
        finte_strlcpy(p->type_name, type_name, sizeof(p->type_name));
    }
    return 0;
}

static int scan_gpt(fdisk_dev_t *d, const uint8_t *mbr)
{
    uint8_t hdr[512];
    if (disk_read_sector(d, 1, hdr) != 0) {
        return -1;
    }
    if (finte_memcmp(hdr, "EFI PART", 8) != 0) {
        return -1;
    }
    uint32_t hsize = rd32(hdr + 12);
    if (hsize < 92 || hsize > 512) {
        return -1;
    }
    uint64_t entries_lba = rd64(hdr + 72);
    uint32_t count = rd32(hdr + 80);
    uint32_t esize = rd32(hdr + 84);
    if (esize < 128 || esize > 512 || count == 0) {
        return -1;
    }
    if (count > 128) {
        count = 128;
    }
    uint32_t per_sector = 512 / esize;
    if (per_sector == 0) {
        per_sector = 1;
    }
    d->scheme = 2;
    uint8_t entry[512];
    uint64_t next_lba = entries_lba;
    for (uint32_t i = 0; i < count && d->part_count < FPART_MAX; i++) {
        uint32_t slot = i % per_sector;
        if (slot == 0 || i == 0) {
            if (disk_read_sector(d, next_lba, entry) != 0) {
                break;
            }
            next_lba++;
        }
        const uint8_t *e = entry + (size_t)slot * esize;
        static const uint8_t zero[16] = { 0 };
        if (guid_eq(e, zero)) {
            continue;
        }
        uint64_t start = rd64(e + 32);
        uint64_t end = rd64(e + 40);
        if (end < start) {
            continue;
        }
        char label[FDISK_LABEL_MAX];
        char type[FDISK_TYPE_MAX];
        gpt_label(e + 56, label, sizeof(label));
        guid_name(e, type, sizeof(type));
        add_part(d, (uint8_t)(i + 1), 2, 0, start,
                 end - start + 1, label[0] ? label : NULL, type);
    }
    (void)mbr;
    return d->part_count ? 0 : -1;
}

static int scan_mbr(fdisk_dev_t *d, const uint8_t *mbr)
{
    d->scheme = 1;
    uint8_t ebr[512];
    int logical = 5;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start = rd32(e + 8);
        uint32_t sectors = rd32(e + 12);
        if (type == 0 || sectors == 0) {
            continue;
        }
        if (type == 0x0F || type == 0x05 || type == 0x85) {
            uint64_t ext_base = start;
            uint64_t ebr_lba = ext_base;
            for (int chain = 0; chain < 32 && d->part_count < FPART_MAX; chain++) {
                if (disk_read_sector(d, ebr_lba, ebr) != 0) {
                    break;
                }
                const uint8_t *le = ebr + 446;
                uint8_t lt = le[4];
                uint32_t lstart = rd32(le + 8);
                uint32_t lsecs = rd32(le + 12);
                if (lt && lsecs) {
                    char tn[FDISK_TYPE_MAX];
                    char label[16];
                    part_name_mbr(lt, tn, sizeof(tn));
                    finte_format(label, sizeof(label), "logical %d", logical);
                    add_part(d, (uint8_t)logical, 1, lt,
                             ext_base + lstart, lsecs, label, tn);
                    logical++;
                }
                uint8_t nt = ebr[446 + 16 + 4];
                uint32_t nstart = rd32(ebr + 446 + 16 + 8);
                if (nt == 0 || nstart == 0) {
                    break;
                }
                ebr_lba = ext_base + nstart;
            }
            continue;
        }
        char tn[FDISK_TYPE_MAX];
        char label[16];
        part_name_mbr(type, tn, sizeof(tn));
        finte_format(label, sizeof(label), "partition %d", i + 1);
        add_part(d, (uint8_t)(i + 1), 1, type, start, sectors, label, tn);
    }
    return d->part_count ? 0 : -1;
}

static void scan_whole(fdisk_dev_t *d)
{
    d->scheme = 0;
    d->part_count = 0;
    add_part(d, 0, 0, 0xFF, 0, d->sectors, "whole disk", "raw");
}

static void scan_device(fdisk_dev_t *d)
{
    uint8_t mbr[512];
    if (disk_read_sector(d, 0, mbr) != 0) {
        scan_whole(d);
        return;
    }
    int sig = rd16(mbr + 510) == 0xAA55;
    int protective = 0;
    if (sig) {
        for (int i = 0; i < 4; i++) {
            if (mbr[446 + i * 16 + 4] == 0xEE) {
                protective = 1;
            }
        }
    }
    if (protective && scan_gpt(d, mbr) == 0) {
        return;
    }
    if (sig && scan_mbr(d, mbr) == 0) {
        return;
    }
    scan_whole(d);
}

void finte_disk_rescan(void)
{
    for (int i = 0; i < FMOUNT_MAX; i++) {
        if (g_mounts[i].used) {
            fdfs_t *fs = &g_mounts[i].fs;
            if (fs->ops && fs->ops->umount) {
                fs->ops->umount(fs->ctx);
            }
            finte_memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
        }
    }
    finte_memset(g_disks, 0, sizeof(g_disks));
    g_disk_count = 0;

    int n = hal_blk_count();
    if (n > FBLK_MAX) {
        n = FBLK_MAX;
    }
    for (int i = 0; i < n && g_disk_count < FDISK_MAX; i++) {
        fblk_info_t bi;
        if (hal_blk_info(i, &bi) != 0 || bi.sectors == 0 ||
            bi.sector_size != 512) {
            continue;
        }
        fdisk_dev_t *d = &g_disks[g_disk_count];
        finte_memset(d, 0, sizeof(*d));
        d->present = 1;
        d->hal_index = i;
        d->sectors = bi.sectors;
        d->sector_size = bi.sector_size;
        finte_format(d->name, sizeof(d->name), "hd%d", g_disk_count);
        scan_device(d);
        g_disk_count++;
    }
    g_scanned = 1;
}

void finte_disk_init(void)
{
    if (!g_scanned) {
        finte_disk_rescan();
    }
}

int finte_disk_count(void)
{
    finte_disk_init();
    return g_disk_count;
}

const fdisk_dev_t *finte_disk_get(int index)
{
    finte_disk_init();
    if (index < 0 || index >= g_disk_count) {
        return NULL;
    }
    return &g_disks[index];
}

const fdisk_part_t *finte_disk_part(int disk, int part)
{
    const fdisk_dev_t *d = finte_disk_get(disk);
    if (!d || part < 0 || part >= d->part_count) {
        return NULL;
    }
    return &d->parts[part];
}

/* ---- filesystem helpers ------------------------------------------------ */

const fdfs_ops_t *fdfs_find(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(g_fs) / sizeof(g_fs[0]); i++) {
        if (finte_strcmp(g_fs[i]->name, name) == 0) {
            return g_fs[i];
        }
    }
    return NULL;
}

int fdfs_read_sectors(fdfs_t *fs, uint64_t lba, void *buf, uint32_t sectors)
{
    if (sectors == 0) {
        return 0;
    }
    if (lba >= fs->sectors || sectors > fs->sectors - lba) {
        return FINTE_ERR_RANGE;
    }
    if (sectors == 8 && (lba & 7u) == 0 && fs->sector_size == 512) {
        fcache_ent_t *e = cache_lookup(fs->disk, fs->start, lba);
        if (!e) {
            e = cache_slot();
            e->valid = 0;
            if (hal_blk_read(fs->disk, fs->start + lba, e->data, 8) != 0) {
                return FINTE_ERR_IO;
            }
            e->valid = 1;
            e->disk = fs->disk;
            e->start = fs->start;
            e->lba = lba;
            e->stamp = ++g_cache_tick;
        }
        finte_memcpy(buf, e->data, 4096);
        return 0;
    }
    return hal_blk_read(fs->disk, fs->start + lba, buf, sectors);
}

int fdfs_write_sectors(fdfs_t *fs, uint64_t lba, const void *buf,
                       uint32_t sectors)
{
    if (sectors == 0) {
        return 0;
    }
    if (lba >= fs->sectors || sectors > fs->sectors - lba) {
        return FINTE_ERR_RANGE;
    }
    cache_invalidate(fs);
    return hal_blk_write(fs->disk, fs->start + lba, buf, sectors);
}

int fdfs_read_bytes(fdfs_t *fs, uint64_t off, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t ss = fs->sector_size;
    while (len) {
        uint64_t lba = off / ss;
        uint32_t in = (uint32_t)(off % ss);
        if (in == 0) {
            uint32_t secs = (uint32_t)(len / ss);
            if (secs > 128) {
                secs = 128;
            }
            if (secs) {
                if (fdfs_read_sectors(fs, lba, out, secs) != 0) {
                    return FINTE_ERR_IO;
                }
                uint64_t adv = (uint64_t)secs * ss;
                off += adv;
                out += adv;
                len -= (size_t)adv;
                continue;
            }
        }
        uint32_t take = ss - in;
        if (take > len) {
            take = (uint32_t)len;
        }
        if (fdfs_read_sectors(fs, lba, g_bounce, 1) != 0) {
            return FINTE_ERR_IO;
        }
        finte_memcpy(out, g_bounce + in, take);
        off += take;
        out += take;
        len -= take;
    }
    return 0;
}

int fdfs_write_bytes(fdfs_t *fs, uint64_t off, const void *buf, size_t len)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t ss = fs->sector_size;
    while (len) {
        uint64_t lba = off / ss;
        uint32_t at = (uint32_t)(off % ss);
        if (at == 0) {
            uint32_t secs = (uint32_t)(len / ss);
            if (secs > 128) {
                secs = 128;
            }
            if (secs) {
                if (fdfs_write_sectors(fs, lba, in, secs) != 0) {
                    return FINTE_ERR_IO;
                }
                uint64_t adv = (uint64_t)secs * ss;
                off += adv;
                in += adv;
                len -= (size_t)adv;
                continue;
            }
        }
        uint32_t take = ss - at;
        if (take > len) {
            take = (uint32_t)len;
        }
        if (fdfs_read_sectors(fs, lba, g_bounce, 1) != 0) {
            return FINTE_ERR_IO;
        }
        finte_memcpy(g_bounce + at, in, take);
        if (fdfs_write_sectors(fs, lba, g_bounce, 1) != 0) {
            return FINTE_ERR_IO;
        }
        off += take;
        in += take;
        len -= take;
    }
    return 0;
}

int fdfs_split_next(const char **path, char *name, size_t name_size)
{
    const char *p = *path;
    while (*p == '/') {
        p++;
    }
    if (*p == 0) {
        *path = p;
        return 0;
    }
    size_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 >= name_size) {
            return FINTE_ERR_INVAL;
        }
        name[n++] = *p++;
    }
    name[n] = 0;
    *path = p;
    return 1;
}

int fdfs_mount_as(fdfs_t *fs, const char *name)
{
    const fdfs_ops_t *ops = fdfs_find(name);
    if (!ops) {
        return FINTE_ERR_TYPE;
    }
    fs->ops = NULL;
    fs->ctx = NULL;
    int rc = ops->mount(fs);
    if (rc != 0) {
        fs->ctx = NULL;
        return rc;
    }
    fs->ops = ops;
    return 0;
}

int fdfs_probe(fdfs_t *fs)
{
    for (size_t i = 0; i < sizeof(g_fs) / sizeof(g_fs[0]); i++) {
        fs->ops = NULL;
        fs->ctx = NULL;
        if (g_fs[i]->mount(fs) == 0) {
            fs->ops = g_fs[i];
            return 0;
        }
        fs->ctx = NULL;
    }
    return FINTE_ERR_TYPE;
}

static int split_device(const char *source, int *disk, int *part)
{
    if (!source || finte_strncmp(source, "hd", 2) != 0) {
        return FINTE_ERR_INVAL;
    }
    int d = 0;
    const char *p = source + 2;
    if (*p < '0' || *p > '9') {
        return FINTE_ERR_INVAL;
    }
    while (*p >= '0' && *p <= '9') {
        d = d * 10 + (*p - '0');
        p++;
    }
    int pi = -1;
    if (*p == 'p') {
        p++;
        if (*p < '1' || *p > '9') {
            return FINTE_ERR_INVAL;
        }
        pi = 0;
        while (*p >= '0' && *p <= '9') {
            pi = pi * 10 + (*p - '0');
            p++;
        }
        pi--;
    }
    if (*p != '\0') {
        return FINTE_ERR_INVAL;
    }
    *disk = d;
    *part = pi;
    return 0;
}

static int find_mount(const char *path, fdfs_t **out, const char **rel)
{
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < FMOUNT_MAX; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        size_t n = finte_strlen(g_mounts[i].target);
        if (finte_strncmp(path, g_mounts[i].target, n) != 0) {
            continue;
        }
        if (!(n == 1 && g_mounts[i].target[0] == '/') &&
            path[n] != '\0' && path[n] != '/') {
            continue;
        }
        if (n >= best_len) {
            best = i;
            best_len = n;
        }
    }
    if (best < 0) {
        return FINTE_ERR_NOENT;
    }
    const char *r = path + best_len;
    while (*r == '/') {
        r++;
    }
    *out = &g_mounts[best].fs;
    *rel = r;
    return 0;
}

int finte_disk_mount(const char *source, const char *target,
                     const char *fstype)
{
    if (!source || !target || target[0] != '/') {
        return FINTE_ERR_INVAL;
    }
    int disk = 0, part = -1;
    int rc = split_device(source, &disk, &part);
    if (rc != 0) {
        return rc;
    }
    const fdisk_dev_t *d = finte_disk_get(disk);
    if (!d) {
        return FINTE_ERR_NOENT;
    }
    const fdisk_part_t *p = NULL;
    if (part >= 0) {
        /* Match the visible partition number (GPT slots and MBR primary
         * numbers can have gaps). */
        for (int j = 0; j < d->part_count; j++) {
            if (d->parts[j].number == (uint8_t)(part + 1)) {
                p = &d->parts[j];
                break;
            }
        }
        if (!p) {
            p = finte_disk_part(disk, part);
        }
    } else if (d->part_count == 1 && d->parts[0].number == 0) {
        p = &d->parts[0];
    }
    if (!p) {
        return FINTE_ERR_NOENT;
    }

    char tgt[FDISK_PATH_MAX];
    finte_strlcpy(tgt, target, sizeof(tgt));
    size_t tl = finte_strlen(tgt);
    while (tl > 1 && tgt[tl - 1] == '/') {
        tgt[--tl] = 0;
    }
    for (int i = 0; i < FMOUNT_MAX; i++) {
        if (g_mounts[i].used && finte_strcmp(g_mounts[i].target, tgt) == 0) {
            return FINTE_ERR_BUSY;
        }
    }

    int slot = -1;
    for (int i = 0; i < FMOUNT_MAX; i++) {
        if (!g_mounts[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return FINTE_ERR_LIMIT;
    }

    fmount_t *m = &g_mounts[slot];
    finte_memset(m, 0, sizeof(*m));
    m->fs.disk = d->hal_index;
    m->fs.start = p->start;
    m->fs.sectors = p->sectors;
    m->fs.sector_size = d->sector_size;
    if (fstype && fstype[0]) {
        rc = fdfs_mount_as(&m->fs, fstype);
    } else {
        rc = fdfs_probe(&m->fs);
    }
    if (rc != 0) {
        finte_memset(m, 0, sizeof(*m));
        return rc;
    }
    m->used = 1;
    finte_strlcpy(m->source, source, sizeof(m->source));
    finte_strlcpy(m->target, tgt, sizeof(m->target));
    return 0;
}

int finte_disk_umount(const char *target)
{
    if (!target) {
        return FINTE_ERR_INVAL;
    }
    for (int i = 0; i < FMOUNT_MAX; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        size_t n = finte_strlen(g_mounts[i].target);
        if (finte_strcmp(g_mounts[i].target, target) == 0 ||
            (finte_strncmp(target, g_mounts[i].target, n) == 0 &&
             target[n] == '/')) {
            fdfs_t *fs = &g_mounts[i].fs;
            if (fs->ops && fs->ops->umount) {
                fs->ops->umount(fs->ctx);
            }
            finte_memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            return 0;
        }
    }
    return FINTE_ERR_NOENT;
}

int finte_disk_mount_list(fmount_info_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < FMOUNT_MAX && n < max; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        finte_memset(&out[n], 0, sizeof(out[n]));
        finte_strlcpy(out[n].source, g_mounts[i].source,
                      sizeof(out[n].source));
        finte_strlcpy(out[n].target, g_mounts[i].target,
                      sizeof(out[n].target));
        finte_strlcpy(out[n].fstype,
                      g_mounts[i].fs.ops ? g_mounts[i].fs.ops->name : "?",
                      sizeof(out[n].fstype));
        n++;
    }
    return n;
}

int finte_disk_ls(const char *path, fdfs_dir_cb cb, void *ctx)
{
    if (!path || path[0] != '/') {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0) {
        return rc;
    }
    return fs->ops->ls(fs->ctx, rel, cb, ctx);
}

int finte_disk_stat(const char *path, fdfs_stat_t *out)
{
    if (!path || path[0] != '/' || !out) {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0) {
        return rc;
    }
    return fs->ops->stat(fs->ctx, rel, out);
}

int64_t finte_disk_read(const char *path, uint64_t off, void *buf, size_t len)
{
    if (!path || path[0] != '/' || (!buf && len)) {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0) {
        return rc;
    }
    return fs->ops->read(fs->ctx, rel, off, buf, len);
}

int64_t finte_disk_write(const char *path, uint64_t off, const void *buf,
                         size_t len)
{
    if (!path || path[0] != '/' || (!buf && len)) {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0 || !fs->ops->write) {
        return rc != 0 ? rc : FINTE_ERR_PERM;
    }
    return fs->ops->write(fs->ctx, rel, off, buf, len);
}

int finte_disk_create(const char *path, int is_dir)
{
    if (!path || path[0] != '/') {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0 || !fs->ops->create) {
        return rc != 0 ? rc : FINTE_ERR_PERM;
    }
    return fs->ops->create(fs->ctx, rel, is_dir);
}

int finte_disk_unlink(const char *path)
{
    if (!path || path[0] != '/') {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0 || !fs->ops->unlink) {
        return rc != 0 ? rc : FINTE_ERR_PERM;
    }
    return fs->ops->unlink(fs->ctx, rel);
}

int finte_disk_rename(const char *from, const char *to)
{
    if (!from || !to || from[0] != '/' || to[0] != '/') {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs1 = NULL, *fs2 = NULL;
    const char *r1 = NULL, *r2 = NULL;
    int rc = find_mount(from, &fs1, &r1);
    if (rc != 0) {
        return rc;
    }
    rc = find_mount(to, &fs2, &r2);
    if (rc != 0) {
        return rc;
    }
    if (fs1 != fs2 || !fs1->ops->rename) {
        return FINTE_ERR_PERM;
    }
    return fs1->ops->rename(fs1->ctx, r1, r2);
}

int finte_disk_truncate(const char *path, uint64_t size)
{
    if (!path || path[0] != '/') {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0 || !fs->ops->truncate) {
        return rc != 0 ? rc : FINTE_ERR_PERM;
    }
    return fs->ops->truncate(fs->ctx, rel, size);
}

int finte_disk_sync(void)
{
    int rc = 0;
    for (int i = 0; i < FMOUNT_MAX; i++) {
        if (!g_mounts[i].used || !g_mounts[i].fs.ops ||
            !g_mounts[i].fs.ops->sync) {
            continue;
        }
        int r = g_mounts[i].fs.ops->sync(g_mounts[i].fs.ctx);
        if (r != 0 && rc == 0) {
            rc = r;
        }
    }
    return rc;
}

int finte_disk_info(const char *path, fdfs_info_t *out)
{
    if (!path || path[0] != '/' || !out) {
        return FINTE_ERR_INVAL;
    }
    fdfs_t *fs = NULL;
    const char *rel = NULL;
    int rc = find_mount(path, &fs, &rel);
    if (rc != 0) {
        return rc;
    }
    (void)rel;
    finte_memset(out, 0, sizeof(*out));
    return fs->ops->info(fs->ctx, out);
}

const char *finte_disk_error(int err)
{
    static const char *const messages[] = {
        "ok",
        "invalid argument",
        "not found",
        "out of memory",
        "permission denied",
        "busy",
        "disk error",
        "limit reached",
        "I/O error",
        "disk error",
        "disk error",
        "disk error",
        "out of range",
        "already exists",
        "unknown filesystem",
        "disk error",
        "end of file"
    };
    if (err > 0 || err <= -(int)FINTE_ARRAY_SIZE(messages)) {
        return "disk error";
    }
    return messages[-err];
}
