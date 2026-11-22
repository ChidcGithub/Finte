/* Kernel block devices: legacy ATA PIO and AHCI DMA.
 *
 * ATA PIO is used on BIOS/SeaBIOS systems. AHCI is used on q35/UEFI and
 * modern SATA controllers. Both expose the same hal_blk_* interface. */
#include "finte/finte.h"
#include "finte/uefi.h"
#include "blk.h"
#include "hal_x86.h"

#define ATA_MAX 4
#define AHCI_MAX_DEV 8

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void io_delay(void)
{
    for (int i = 0; i < 4; i++) {
        (void)inb(0x80);
    }
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void parse_identify(const uint16_t *id, uint64_t *sectors,
                           uint32_t *sector_size, int *lba48)
{
    uint64_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    uint64_t lba48v = (uint32_t)id[100] | ((uint32_t)id[101] << 16) |
                      ((uint64_t)(uint32_t)id[102] << 32) |
                      ((uint64_t)(uint32_t)id[103] << 48);
    int has48 = (id[83] & (1u << 10)) != 0;
    *lba48 = has48;
    *sectors = has48 ? lba48v : lba28;
    *sector_size = 512;
    if ((id[106] & (1u << 12)) && id[117] && id[118]) {
        uint32_t ss = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (ss >= 512 && ss <= 4096 && (ss & (ss - 1)) == 0) {
            *sector_size = ss;
        }
    }
}

/* ---- ATA PIO ----------------------------------------------------------- */

typedef struct ata_dev {
    int used;
    uint16_t base;
    uint16_t ctrl;
    uint8_t slave;
    int lba48;
    uint32_t sector_size;
    uint64_t sectors;
    char name[FBLK_NAME_MAX];
} ata_dev_t;

static ata_dev_t g_ata[ATA_MAX];
static int g_ata_count;

static int ata_wait(uint16_t base, uint8_t mask, uint8_t want, int limit)
{
    for (int i = 0; i < limit; i++) {
        uint8_t st = inb(base + 7);
        if (st == 0xFF) {
            return -1;
        }
        if ((st & 0x80) == 0 && (st & mask) == want) {
            return 0;
        }
    }
    return -1;
}

static int ata_identify(uint16_t base, uint16_t ctrl, int slave,
                        uint16_t *id)
{
    (void)ctrl;
    outb(base + 6, (uint8_t)(0xA0 | (slave ? 0x10 : 0)));
    io_delay();
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);
    outb(base + 7, 0xEC);
    io_delay();
    uint8_t st = inb(base + 7);
    if (st == 0 || st == 0xFF) {
        return -1;
    }
    if (ata_wait(base, 0x80, 0, 1000000) != 0) {
        return -1;
    }
    st = inb(base + 7);
    if (st & 0x01) {
        return -1;
    }
    if (ata_wait(base, 0x08, 0x08, 1000000) != 0) {
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        id[i] = inw(base);
    }
    return 0;
}

static int ata_read(ata_dev_t *d, uint64_t lba, void *buf, uint32_t sectors)
{    if (!d->used || sectors == 0 || sectors > 128) {
        return -1;
    }
    if (lba >= d->sectors || sectors > d->sectors - lba) {
        return -1;
    }
    int use48 = d->lba48;
    outb(d->base + 6, (uint8_t)((use48 ? 0x40 : 0xE0) |
                                 (d->slave ? 0x10 : 0)));
    io_delay();
    if (use48) {
        outb(d->base + 2, (uint8_t)(sectors >> 8));
        outb(d->base + 3, (uint8_t)(lba >> 24));
        outb(d->base + 4, (uint8_t)(lba >> 32));
        outb(d->base + 5, (uint8_t)(lba >> 40));
        outb(d->base + 2, (uint8_t)(sectors & 0xFF));
        outb(d->base + 3, (uint8_t)(lba & 0xFF));
        outb(d->base + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(d->base + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(d->base + 7, 0x24);
    } else {
        outb(d->base + 2, (uint8_t)(sectors & 0xFF));
        outb(d->base + 3, (uint8_t)(lba & 0xFF));
        outb(d->base + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(d->base + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(d->base + 7, 0x20);
    }
    uint16_t *out = (uint16_t *)buf;
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_wait(d->base, 0x80, 0, 2000000) != 0) {
            return -1;
        }
        uint8_t st = inb(d->base + 7);
        if (st & 0x01) {
            return -1;
        }
        if (ata_wait(d->base, 0x08, 0x08, 2000000) != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            *out++ = inw(d->base);
        }
    }
    return 0;
}

static int ata_write(ata_dev_t *d, uint64_t lba, const void *buf,
                     uint32_t sectors)
{
    if (!d->used || sectors == 0 || sectors > 128) {
        return -1;
    }
    if (lba >= d->sectors || sectors > d->sectors - lba) {
        return -1;
    }
    int use48 = d->lba48;
    outb(d->base + 6, (uint8_t)((use48 ? 0x40 : 0xE0) |
                                 (d->slave ? 0x10 : 0)));
    io_delay();
    if (use48) {
        outb(d->base + 2, (uint8_t)(sectors >> 8));
        outb(d->base + 3, (uint8_t)(lba >> 24));
        outb(d->base + 4, (uint8_t)(lba >> 32));
        outb(d->base + 5, (uint8_t)(lba >> 40));
        outb(d->base + 2, (uint8_t)(sectors & 0xFF));
        outb(d->base + 3, (uint8_t)(lba & 0xFF));
        outb(d->base + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(d->base + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(d->base + 7, 0x34);
    } else {
        outb(d->base + 2, (uint8_t)(sectors & 0xFF));
        outb(d->base + 3, (uint8_t)(lba & 0xFF));
        outb(d->base + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(d->base + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(d->base + 7, 0x30);
    }
    const uint16_t *in = (const uint16_t *)buf;
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_wait(d->base, 0x80, 0, 2000000) != 0) {
            return -1;
        }
        uint8_t st = inb(d->base + 7);
        if (st & 0x01) {
            return -1;
        }
        if (ata_wait(d->base, 0x08, 0x08, 2000000) != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            outw(d->base, *in++);
        }
    }
    if (ata_wait(d->base, 0x80, 0, 2000000) != 0) {
        return -1;
    }
    return (inb(d->base + 7) & 0x01) ? -1 : 0;
}

static void ata_scan(void)
{
    static const struct {
        uint16_t base;
        uint16_t ctrl;
    } ch[2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };

    for (int c = 0; c < 2 && g_ata_count < ATA_MAX; c++) {
        for (int s = 0; s < 2 && g_ata_count < ATA_MAX; s++) {
            uint16_t id[256];
            if (ata_identify(ch[c].base, ch[c].ctrl, s, id) != 0) {
                continue;
            }
            if (id[0] & 0x8000) {
                continue; /* ATAPI */
            }
            ata_dev_t *d = &g_ata[g_ata_count];
            finte_memset(d, 0, sizeof(*d));
            d->used = 1;
            d->base = ch[c].base;
            d->ctrl = ch[c].ctrl;
            d->slave = (uint8_t)s;
            parse_identify(id, &d->sectors, &d->sector_size, &d->lba48);
            if (d->sectors == 0) {
                d->used = 0;
                continue;
            }
            finte_format(d->name, sizeof(d->name), "ata%d", g_ata_count);
            g_ata_count++;
        }
    }
}

/* ---- AHCI -------------------------------------------------------------- */

typedef struct ahci_dev {
    int used;
    volatile uint32_t *port;
    uint32_t slots;
    int lba48;
    uint32_t sector_size;
    uint64_t sectors;
    uint8_t *cl;
    uint8_t *fis;
    uint8_t *ct;
    uint8_t *idbuf;
    char name[FBLK_NAME_MAX];
} ahci_dev_t;

static ahci_dev_t g_ahci[AHCI_MAX_DEV];
static int g_ahci_count;
static fboot_blk_t g_fw[FINTE_BOOT_BLK_MAX];
static int g_fw_count;
static uint8_t g_cl[AHCI_MAX_DEV][1024] __attribute__((aligned(1024)));
static uint8_t g_fis[AHCI_MAX_DEV][256] __attribute__((aligned(256)));
static uint8_t g_ct[AHCI_MAX_DEV][1024] __attribute__((aligned(128)));
static uint8_t g_id[AHCI_MAX_DEV][512] __attribute__((aligned(16)));

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint16_t value)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFC);
    outl(0xCF8, addr);
    outw(0xCFC, value);
}

static int ahci_wait_ci(ahci_dev_t *d, uint32_t slot)
{
    uint64_t limit = hal_cycles() + hal_cycles_per_sec() * 2ull;
    while (hal_cycles() < limit) {
        if ((d->port[0x38 / 4] & (1u << slot)) == 0) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int ahci_cmd(ahci_dev_t *d, uint8_t cmd, uint64_t lba,
                    uint16_t count, void *buf, int write)
{
    uint32_t bytes = (uint32_t)count * 512u;
    uint32_t slot = 0;
    if (d->port[0x38 / 4] != 0) {
        if (ahci_wait_ci(d, slot) != 0) {
            return -1;
        }
    }
    finte_memset(d->cl, 0, 32);
    finte_memset(d->ct, 0, 0x80 + 16);

    uint32_t *hdr = (uint32_t *)(void *)d->cl;
    hdr[0] = 5u | ((write ? 1u : 0u) << 6) | (1u << 16);
    hdr[2] = (uint32_t)(uintptr_t)d->ct;
    hdr[3] = (uint32_t)((uint64_t)(uintptr_t)d->ct >> 32);

    uint8_t *f = d->ct;
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = cmd;
    if (cmd == 0xEC) {
        f[7] = 0;
    } else {
        f[4] = (uint8_t)lba;
        f[5] = (uint8_t)(lba >> 8);
        f[6] = (uint8_t)(lba >> 16);
        f[7] = 0x40;
        f[8] = (uint8_t)(lba >> 24);
        f[9] = (uint8_t)(lba >> 32);
        f[10] = (uint8_t)(lba >> 40);
        f[12] = (uint8_t)(count & 0xFF);
        f[13] = (uint8_t)(count >> 8);
    }

    uint8_t *prdt = d->ct + 0x80;
    uint64_t addr = (uint64_t)(uintptr_t)buf;
    for (int i = 0; i < 8; i++) {
        prdt[i] = (uint8_t)(addr >> (8 * i));
    }
    prdt[8] = prdt[9] = prdt[10] = prdt[11] = 0; /* reserved */
    uint32_t dbc = (bytes - 1u) & 0x3FFFFFu;
    prdt[12] = (uint8_t)dbc;
    prdt[13] = (uint8_t)(dbc >> 8);
    prdt[14] = (uint8_t)(dbc >> 16);
    prdt[15] = (uint8_t)(dbc >> 24);

    d->port[0x10 / 4] = 0xFFFFFFFFu;
    d->port[0x30 / 4] = 0xFFFFFFFFu;
    __asm__ volatile("mfence" ::: "memory");
    d->port[0x38 / 4] = 1u << slot;
    if (ahci_wait_ci(d, slot) != 0) {
        return -1;
    }
    uint32_t is = d->port[0x10 / 4];
    uint32_t tfd = d->port[0x20 / 4];
    if ((is & (1u << 30)) || (tfd & 0x21)) {
        return -1;
    }
    return 0;
}

static int ahci_identify(ahci_dev_t *d, uint16_t *id)
{
    if (ahci_cmd(d, 0xEC, 0, 1, d->idbuf, 0) != 0) {
        return -1;
    }
    const uint8_t *p = d->idbuf;
    for (int i = 0; i < 256; i++) {
        id[i] = rd16(p + i * 2);
    }
    return 0;
}

static void ahci_init_port(volatile uint32_t *mmio, uint32_t port_no,
                           uint32_t idx)
{
    volatile uint32_t *p = (volatile uint32_t *)((uint8_t *)mmio +
                                                 0x100 + port_no * 0x80);
    uint32_t ssts = p[0x28 / 4];
    uint32_t sig = p[0x24 / 4];
    if ((ssts & 0xF) != 3 || sig != 0x00000101u) {
        return;
    }
    uint32_t cmd = p[0x18 / 4];
    if (cmd & 1u) {
        p[0x18 / 4] = cmd & ~1u;
        for (int i = 0; i < 100000 && (p[0x18 / 4] & (1u << 15)); i++) {
            __asm__ volatile("pause");
        }
    }
    /* Reset the device so firmware-left command state cannot block us. */
    p[0x2C / 4] = 1; /* PxSCTL.DET = 1 */
    for (volatile int i = 0; i < 200000; i++) {
    }
    p[0x2C / 4] = 0;
    for (int i = 0; i < 2000000; i++) {
        if ((p[0x28 / 4] & 0xF) == 3) {
            break;
        }
        __asm__ volatile("pause");
    }
    p[0x10 / 4] = 0xFFFFFFFFu;
    p[0x30 / 4] = 0xFFFFFFFFu;
    p[0x00 / 4] = (uint32_t)(uintptr_t)g_cl[idx];
    p[0x04 / 4] = (uint32_t)((uint64_t)(uintptr_t)g_cl[idx] >> 32);
    p[0x08 / 4] = (uint32_t)(uintptr_t)g_fis[idx];
    p[0x0C / 4] = (uint32_t)((uint64_t)(uintptr_t)g_fis[idx] >> 32);
    p[0x38 / 4] = 0;
    p[0x18 / 4] = 1u << 4; /* FRE */
    for (int i = 0; i < 100000 && (p[0x18 / 4] & (1u << 14)) == 0; i++) {
        __asm__ volatile("pause");
    }
    p[0x18 / 4] = (1u << 4) | 1u; /* FRE | ST */
    for (int i = 0; i < 100000 && (p[0x18 / 4] & (1u << 15)) == 0; i++) {
        __asm__ volatile("pause");
    }
    for (volatile int i = 0; i < 100000; i++) {
    }
    ahci_dev_t *d = &g_ahci[idx];
    finte_memset(d, 0, sizeof(*d));
    d->port = p;
    d->cl = g_cl[idx];
    d->fis = g_fis[idx];
    d->ct = g_ct[idx];
    d->idbuf = g_id[idx];
    uint16_t id[256];
    if (ahci_identify(d, id) != 0 || (id[0] & 0x8000)) {
        return;
    }
    parse_identify(id, &d->sectors, &d->sector_size, &d->lba48);
    if (d->sectors == 0) {
        return;
    }
    d->used = 1;
    finte_format(d->name, sizeof(d->name), "ahci%d", idx);
    g_ahci_count++;
}

static void ahci_scan(void)
{
    if (!hal_is_uefi()) {
        /* BIOS builds only identity-map the first 1 GiB. AHCI BARs usually
         * sit above that, so only probe AHCI under UEFI page tables. */
        return;
    }
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev,
                                          (uint8_t)fn, 0x08);
                if (((cls >> 24) & 0xFF) != 0x01 ||
                    ((cls >> 16) & 0xFF) != 0x06 ||
                    ((cls >> 8) & 0xFF) != 0x01) {
                    continue;
                }
                uint32_t raw_bar = pci_read32((uint8_t)bus, (uint8_t)dev,
                                              (uint8_t)fn, 0x24);
                if (raw_bar & 1u) {
                    continue;
                }
                uint32_t bar_type = (raw_bar >> 1) & 3u;
                uint64_t abar = (uint64_t)(raw_bar & ~0xFu);
                if (bar_type == 2) {
                    abar |= (uint64_t)pci_read32((uint8_t)bus, (uint8_t)dev,
                                                 (uint8_t)fn, 0x28) << 32;
                }
                if (abar == 0) {
                    continue;
                }
                uint16_t cmd = (uint16_t)pci_read32((uint8_t)bus,
                                                    (uint8_t)dev,
                                                    (uint8_t)fn, 0x04);
                pci_write16((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x04,
                            (uint16_t)(cmd | 0x6));
                volatile uint32_t *mmio = (volatile uint32_t *)(uintptr_t)abar;
                uint32_t cap = mmio[0];
                mmio[1] |= 1u << 31; /* AHCI enable */
                uint32_t ports = (cap & 0x1F) + 1;
                if (ports > 32) {
                    ports = 32;
                }
                uint32_t pi = mmio[3];
                for (uint32_t p = 0; p < ports && g_ahci_count < AHCI_MAX_DEV;
                     p++) {
                    if ((pi & (1u << p)) == 0) {
                        continue;
                    }
                    ahci_init_port(mmio, p, (uint32_t)g_ahci_count);
                }
                return;
            }
        }
    }
}

/* ---- public HAL block interface --------------------------------------- */

static int g_blk_init;
static int g_blk_total;

void blk_boot_info(const fboot_info_t *bi)
{
    finte_memset(g_fw, 0, sizeof(g_fw));
    g_fw_count = 0;
    if (!bi || bi->magic != FINTE_BOOT_MAGIC ||
        bi->console_type != FINTE_BOOT_CONSOLE_UEFI || !bi->fw_services) {
        return;
    }
    uint32_t n = bi->blk_count;
    if (n > FINTE_BOOT_BLK_MAX) {
        n = FINTE_BOOT_BLK_MAX;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (bi->blk[i].block_io && bi->blk[i].block_size == 512 &&
            bi->blk[i].last_block) {
            g_fw[g_fw_count++] = bi->blk[i];
        }
    }
}

static void blk_init(void)
{
    if (g_blk_init) {
        return;
    }
    g_blk_init = 1;
    ata_scan();
    ahci_scan();
    /* OVMF exposes the same SATA disks through Block I/O. Keep firmware
     * devices only when native ATA/AHCI did not already enumerate them;
     * this leaves NVMe/USB storage available without duplicate hd entries. */
    int out = 0;
    for (int i = 0; i < g_fw_count; i++) {
        uint64_t sectors = g_fw[i].last_block;
        int dup = 0;
        for (int a = 0; a < g_ata_count; a++) {
            if (g_ata[a].sectors == sectors) {
                dup = 1;
            }
        }
        for (int h = 0; h < g_ahci_count; h++) {
            if (g_ahci[h].sectors == sectors) {
                dup = 1;
            }
        }
        if (!dup) {
            g_fw[out++] = g_fw[i];
        }
    }
    g_fw_count = out;
    g_blk_total = g_ata_count + g_ahci_count + g_fw_count;
}

int hal_blk_count(void)
{
    blk_init();
    return g_blk_total;
}

int hal_blk_info(int index, fblk_info_t *out)
{
    blk_init();
    if (!out || index < 0 || index >= g_blk_total) {
        return -1;
    }
    finte_memset(out, 0, sizeof(*out));
    if (index < g_ata_count) {
        ata_dev_t *d = &g_ata[index];
        finte_strlcpy(out->name, d->name, sizeof(out->name));
        out->sectors = d->sectors;
        out->sector_size = d->sector_size;
    } else if (index < g_ata_count + g_ahci_count) {
        ahci_dev_t *d = &g_ahci[index - g_ata_count];
        finte_strlcpy(out->name, d->name, sizeof(out->name));
        out->sectors = d->sectors;
        out->sector_size = d->sector_size;
    } else {
        fboot_blk_t *b = &g_fw[index - g_ata_count - g_ahci_count];
        finte_strlcpy(out->name, b->name, sizeof(out->name));
        out->sectors = b->last_block;
        out->sector_size = b->block_size;
        out->flags = (b->flags & 1u) ? FBLK_FLAG_REMOVABLE : 0;
    }
    return 0;
}

static int fw_read(fboot_blk_t *b, uint64_t lba, void *buf, uint32_t sectors)
{
    if (lba >= b->last_block || sectors > b->last_block - lba) {
        return -1;
    }
    EFI_BLOCK_IO_PROTOCOL *bio =
        (EFI_BLOCK_IO_PROTOCOL *)(uintptr_t)b->block_io;
    UINTN bytes = (UINTN)sectors * b->block_size;
    if (!hal_fw_begin()) {
        return -1;
    }
    EFI_STATUS st = bio->ReadBlocks(bio, b->media_id, lba, bytes, buf);
    hal_fw_end();
    return st == 0 ? 0 : -1;
}

static int fw_write(fboot_blk_t *b, uint64_t lba, const void *buf,
                    uint32_t sectors)
{
    if ((b->flags & 1u) || !b->block_io || lba >= b->last_block ||
        sectors > b->last_block - lba) {
        return -1;
    }
    EFI_BLOCK_IO_PROTOCOL *bio =
        (EFI_BLOCK_IO_PROTOCOL *)(uintptr_t)b->block_io;
    if (!bio->WriteBlocks) {
        return -1;
    }
    UINTN bytes = (UINTN)sectors * b->block_size;
    if (!hal_fw_begin()) {
        return -1;
    }
    EFI_STATUS st = bio->WriteBlocks(bio, b->media_id, lba, bytes, buf);
    hal_fw_end();
    return st == 0 ? 0 : -1;
}

int hal_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors)
{
    blk_init();
    if (index < 0 || index >= g_blk_total) {
        return -1;
    }
    if (index < g_ata_count) {
        return ata_read(&g_ata[index], lba, buf, sectors);
    }
    if (index < g_ata_count + g_ahci_count) {
        ahci_dev_t *d = &g_ahci[index - g_ata_count];
        if (!d->used || sectors == 0 || sectors > 128) {
            return -1;
        }
        return ahci_cmd(d, 0x25, lba, (uint16_t)sectors, buf, 0);
    }
    return fw_read(&g_fw[index - g_ata_count - g_ahci_count], lba, buf,
                   sectors);
}

int hal_blk_write(int index, uint64_t lba, const void *buf, uint32_t sectors)
{
    blk_init();
    if (index < 0 || index >= g_blk_total) {
        return -1;
    }
    if (index < g_ata_count) {
        return ata_write(&g_ata[index], lba, buf, sectors);
    }
    if (index < g_ata_count + g_ahci_count) {
        ahci_dev_t *d = &g_ahci[index - g_ata_count];
        if (!d->used || sectors == 0 || sectors > 128) {
            return -1;
        }
        return ahci_cmd(d, 0x35, lba, (uint16_t)sectors, (void *)buf, 1);
    }
    return fw_write(&g_fw[index - g_ata_count - g_ahci_count], lba, buf,
                    sectors);
}
