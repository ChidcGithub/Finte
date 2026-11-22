/* Disk manager: block devices, partition tables, mounts and read-only
 * filesystems (NTFS, ext4, lfc1). */
#ifndef FINTE_DISK_H
#define FINTE_DISK_H

#include "finte/base.h"
#include "finte/hal.h"

#define FDISK_MAX 8
#define FPART_MAX 16
#define FMOUNT_MAX 8
#define FDISK_NAME_MAX 16
#define FDISK_LABEL_MAX 40
#define FDISK_PATH_MAX 96
#define FDISK_TYPE_MAX 24

#define FDISK_MODE_DIR 1u
#define FDISK_MODE_FILE 2u

typedef struct fdisk_part {
    int present;
    uint8_t number;          /* 0 = whole disk */
    uint8_t scheme;          /* 0 whole, 1 MBR, 2 GPT */
    uint8_t mbr_type;
    uint8_t reserved;
    char type_name[FDISK_TYPE_MAX];
    char label[FDISK_LABEL_MAX];
    uint64_t start;          /* LBA, absolute in the block device */
    uint64_t sectors;
} fdisk_part_t;

typedef struct fdisk_dev {
    int present;
    int hal_index;
    char name[FDISK_NAME_MAX]; /* hd0, hd1, ... */
    uint64_t sectors;
    uint32_t sector_size;
    uint8_t scheme;
    int part_count;
    fdisk_part_t parts[FPART_MAX];
} fdisk_dev_t;

typedef struct fdfs_stat {
    uint64_t size;
    uint32_t mode;           /* FDISK_MODE_DIR or FDISK_MODE_FILE */
    uint32_t reserved;
} fdfs_stat_t;

typedef struct fdfs_info {
    char fstype[8];
    char label[FDISK_LABEL_MAX];
    uint32_t block_size;
    uint64_t total_bytes;
    uint64_t used_bytes;     /* 0 when the format does not track it */
    uint64_t free_bytes;
} fdfs_info_t;

typedef struct fmount_info {
    char source[FDISK_NAME_MAX];
    char target[FDISK_PATH_MAX];
    char fstype[8];
} fmount_info_t;

/* Read-only directory callback. The name is NUL-terminated and only valid for
 * the duration of the call. */
typedef void (*fdfs_dir_cb)(void *ctx, const char *name, const fdfs_stat_t *st);

void finte_disk_init(void);
void finte_disk_rescan(void);
int finte_disk_count(void);
const fdisk_dev_t *finte_disk_get(int index);
const fdisk_part_t *finte_disk_part(int disk, int part);

int finte_disk_mount(const char *source, const char *target,
                     const char *fstype);
int finte_disk_umount(const char *target);
int finte_disk_mount_list(fmount_info_t *out, int max);
int finte_disk_ls(const char *path, fdfs_dir_cb cb, void *ctx);
int finte_disk_stat(const char *path, fdfs_stat_t *out);
int64_t finte_disk_read(const char *path, uint64_t off, void *buf, size_t len);
int64_t finte_disk_write(const char *path, uint64_t off, const void *buf,
                         size_t len);
int finte_disk_create(const char *path, int is_dir);
int finte_disk_unlink(const char *path);
int finte_disk_rename(const char *from, const char *to);
int finte_disk_truncate(const char *path, uint64_t size);
int finte_disk_sync(void);
int finte_disk_info(const char *path, fdfs_info_t *out);
int finte_disk_block_info(const char *name, uint64_t *sectors,
                          uint32_t *sector_size);
int finte_disk_block_read(const char *name, uint64_t lba, void *buf,
                          uint32_t sectors);
int finte_disk_block_write(const char *name, uint64_t lba, const void *buf,
                           uint32_t sectors);
const char *finte_disk_error(int err);

#endif /* FINTE_DISK_H */
