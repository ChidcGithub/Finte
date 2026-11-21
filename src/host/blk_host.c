/* Host block devices: raw image files used by the disk manager and tests. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#else
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#include "finte/finte.h"
#include "hal_host.h"

#include <stdio.h>
#include <string.h>

#define HOST_BLK_MAX 8

typedef struct host_blk {
    int used;
    int writable;
    FILE *f;
    uint64_t sectors;
    uint32_t sector_size;
    char name[FBLK_NAME_MAX];
} host_blk_t;

static host_blk_t g_blk[HOST_BLK_MAX];
static int g_blk_count;

static int seek64(FILE *f, uint64_t off)
{
#if defined(_WIN32)
    return _fseeki64(f, (__int64)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

int hal_blk_add_image(const char *path)
{
    if (!path || g_blk_count >= HOST_BLK_MAX) {
        return -1;
    }
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
    }
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
#if defined(_WIN32)
    __int64 end = _ftelli64(f);
#else
    off_t end = ftello(f);
#endif
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    host_blk_t *b = &g_blk[g_blk_count];
    memset(b, 0, sizeof(*b));
    b->used = 1;
    b->f = f;
    b->sectors = (uint64_t)end / 512u;
    b->sector_size = 512;
    b->writable = 1;
    fseek(f, 0, SEEK_SET);
    snprintf(b->name, sizeof(b->name), "img%d", g_blk_count);
    g_blk_count++;
    return 0;
}

int hal_blk_count(void)
{
    return g_blk_count;
}

int hal_blk_info(int index, fblk_info_t *out)
{
    if (!out || index < 0 || index >= g_blk_count || !g_blk[index].used) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, g_blk[index].name, sizeof(out->name));
    out->sectors = g_blk[index].sectors;
    out->sector_size = g_blk[index].sector_size;
    out->flags = 0;
    return 0;
}

int hal_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors)
{
    if (index < 0 || index >= g_blk_count || !g_blk[index].used ||
        !buf || sectors == 0) {
        return -1;
    }
    host_blk_t *b = &g_blk[index];
    if (lba >= b->sectors || sectors > b->sectors - lba) {
        return -1;
    }
    if (seek64(b->f, lba * b->sector_size) != 0) {
        return -1;
    }
    size_t want = (size_t)sectors * b->sector_size;
    if (fread(buf, 1, want, b->f) != want) {
        return -1;
    }
    return 0;
}

int hal_blk_write(int index, uint64_t lba, const void *buf, uint32_t sectors)
{
    if (index < 0 || index >= g_blk_count || !g_blk[index].used ||
        !buf || sectors == 0 || !g_blk[index].writable) {
        return -1;
    }
    host_blk_t *b = &g_blk[index];
    if (lba >= b->sectors || sectors > b->sectors - lba) {
        return -1;
    }
    if (seek64(b->f, lba * b->sector_size) != 0) {
        return -1;
    }
    size_t want = (size_t)sectors * b->sector_size;
    if (fwrite(buf, 1, want, b->f) != want) {
        return -1;
    }
    fflush(b->f);
    return 0;
}
