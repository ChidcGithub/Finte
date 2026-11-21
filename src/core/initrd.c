/* Read-only ramdisk archive loaded by the bootloader.
 *
 * Layout:
 *   u32 magic, u32 count
 *   repeated: char name[32], u32 size, u8 data[size] padded to 4
 */
#include "finte/initrd.h"

typedef struct initrd_entry {
    const char *name;
    const void *data;
    uint32_t size;
} initrd_entry_t;

static initrd_entry_t g_entries[FINTE_INITRD_MAX];
static uint32_t g_count;
static int g_valid;

int finte_initrd_init(const void *addr, size_t size)
{
    g_count = 0;
    g_valid = 0;
    if (!addr || size < 8) {
        return -1;
    }
    const uint8_t *base = (const uint8_t *)addr;
    uint32_t magic = 0;
    uint32_t count = 0;
    finte_memcpy(&magic, base, 4);
    finte_memcpy(&count, base + 4, 4);
    if (magic != FINTE_INITRD_MAGIC || count > FINTE_INITRD_MAX) {
        return -1;
    }
    size_t off = 8;
    uint32_t stored = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 36 > size) {
            break;
        }
        const char *name = (const char *)(base + off);
        size_t name_len = 0;
        while (name_len < 32 && name[name_len]) {
            name_len++;
        }
        if (name_len == 32) {
            break;
        }
        uint32_t len = 0;
        finte_memcpy(&len, base + off + 32, 4);
        off += 36;
        if (len > size - off) {
            break;
        }
        g_entries[stored].name = name;
        g_entries[stored].data = base + off;
        g_entries[stored].size = len;
        stored++;
        off += (len + 3u) & ~(size_t)3u;
    }
    g_count = stored;
    g_valid = 1;
    return 0;
}

uint32_t finte_initrd_count(void)
{
    return g_valid ? g_count : 0;
}

const char *finte_initrd_name(uint32_t index)
{
    return index < g_count ? g_entries[index].name : NULL;
}

const void *finte_initrd_data(uint32_t index)
{
    return index < g_count ? g_entries[index].data : NULL;
}

uint32_t finte_initrd_size(uint32_t index)
{
    return index < g_count ? g_entries[index].size : 0;
}

int finte_initrd_find(const char *name)
{
    if (!name) {
        return -1;
    }
    for (uint32_t i = 0; i < g_count; i++) {
        if (finte_strcmp(g_entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}
