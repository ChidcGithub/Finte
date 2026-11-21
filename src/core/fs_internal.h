/* Internal filesystem driver interface used by disk.c. */
#ifndef FINTE_FS_INTERNAL_H
#define FINTE_FS_INTERNAL_H

#include "finte/disk.h"

typedef struct fdfs fdfs_t;

typedef struct fdfs_ops {
    const char *name;
    int (*mount)(fdfs_t *fs);
    void (*umount)(void *ctx);
    int (*stat)(void *ctx, const char *rel, fdfs_stat_t *out);
    int (*ls)(void *ctx, const char *rel, fdfs_dir_cb cb, void *cbctx);
    int64_t (*read)(void *ctx, const char *rel, uint64_t off, void *buf,
                    size_t len);
    int (*info)(void *ctx, fdfs_info_t *out);
    int (*create)(void *ctx, const char *rel, int is_dir);
    int (*unlink)(void *ctx, const char *rel);
    int (*rename)(void *ctx, const char *from, const char *to);
    int64_t (*write)(void *ctx, const char *rel, uint64_t off,
                     const void *buf, size_t len);
    int (*truncate)(void *ctx, const char *rel, uint64_t size);
    int (*sync)(void *ctx);
} fdfs_ops_t;

struct fdfs {
    const fdfs_ops_t *ops;
    void *ctx;
    int disk;
    uint32_t sector_size;
    uint64_t start;          /* partition start LBA */
    uint64_t sectors;
};

int fdfs_read_sectors(fdfs_t *fs, uint64_t lba, void *buf, uint32_t sectors);
int fdfs_read_bytes(fdfs_t *fs, uint64_t off, void *buf, size_t len);
int fdfs_write_sectors(fdfs_t *fs, uint64_t lba, const void *buf,
                       uint32_t sectors);
int fdfs_write_bytes(fdfs_t *fs, uint64_t off, const void *buf, size_t len);

const fdfs_ops_t *fdfs_find(const char *name);
int fdfs_probe(fdfs_t *fs);
int fdfs_mount_as(fdfs_t *fs, const char *name);

extern const fdfs_ops_t fdfs_lfc1;
extern const fdfs_ops_t fdfs_ext4;
extern const fdfs_ops_t fdfs_ntfs;

/* Shared path helpers. */
int fdfs_split_next(const char **path, char *name, size_t name_size);

#endif /* FINTE_FS_INTERNAL_H */
