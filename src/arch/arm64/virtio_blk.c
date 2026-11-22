/* Virtio-mmio block driver for QEMU virt.
 *
 * Scans the 32 virtio-mmio transports at 0x0A000000, supports both the
 * legacy (version 1) and modern (version 2) transports, and exposes one
 * polled virtqueue as a 512-byte-sector block device through hal_blk_*.
 */
#include "finte/finte.h"

#define VIO_MMIO_BASE   0x0A000000ull
#define VIO_MMIO_STRIDE 0x200u
#define VIO_MMIO_SLOTS  32u

#define VIO_MAGIC               0x000u
#define VIO_VERSION             0x004u
#define VIO_DEVICE_ID           0x008u
#define VIO_VENDOR_ID           0x00Cu
#define VIO_DEVICE_FEATURES     0x010u
#define VIO_DEVICE_FEATURES_SEL 0x014u
#define VIO_DRIVER_FEATURES     0x020u
#define VIO_DRIVER_FEATURES_SEL 0x024u
#define VIO_GUEST_PAGE_SIZE     0x028u
#define VIO_QUEUE_SEL           0x030u
#define VIO_QUEUE_NUM_MAX       0x034u
#define VIO_QUEUE_NUM           0x038u
#define VIO_QUEUE_ALIGN         0x03Cu
#define VIO_QUEUE_PFN           0x040u
#define VIO_QUEUE_READY         0x044u
#define VIO_QUEUE_NOTIFY        0x050u
#define VIO_STATUS              0x070u
#define VIO_QUEUE_DESC_LOW      0x080u
#define VIO_QUEUE_DESC_HIGH     0x084u
#define VIO_QUEUE_DRIVER_LOW    0x090u
#define VIO_QUEUE_DRIVER_HIGH   0x094u
#define VIO_QUEUE_DEVICE_LOW    0x0A0u
#define VIO_QUEUE_DEVICE_HIGH   0x0A4u
#define VIO_CONFIG              0x100u

#define VIO_MAGIC_VALUE   0x74726976u /* "virt" */
#define VIO_VENDOR_QEMU   0x554D4551u /* "QEMU" */
#define VIO_DEVICE_BLOCK  2u

#define VIO_STATUS_ACK        1u
#define VIO_STATUS_DRIVER     2u
#define VIO_STATUS_DRIVER_OK  4u
#define VIO_STATUS_FEATURES_OK 8u

#define VIRTIO_BLK_F_RO      5u
#define VIRTIO_F_VERSION_1   32u

#define VIO_QUEUE_SIZE 64u
#define VIO_DESC_OFF   0x000u
#define VIO_AVAIL_OFF  0x400u
#define VIO_USED_OFF   0x800u /* modern */
#define VIO_USED_LEGACY_OFF 0x1000u
#define VIO_RING_BYTES 0x2000u

#define VIO_REQ_IN    0u
#define VIO_REQ_OUT   1u
#define VIO_MAX_SECTORS 128u

typedef struct vio_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vio_desc_t;

typedef struct vio_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIO_QUEUE_SIZE];
} vio_avail_t;

typedef struct vio_used_elem {
    uint32_t id;
    uint32_t len;
} vio_used_elem_t;

typedef struct vio_used {
    uint16_t flags;
    uint16_t idx;
    vio_used_elem_t ring[VIO_QUEUE_SIZE];
} vio_used_t;

typedef struct vio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} vio_blk_req_t;

typedef struct vio_dev {
    volatile uint32_t *mmio;
    uint32_t slot;
    uint32_t version;
    uint64_t capacity;
    int readonly;
    vio_desc_t *desc;
    volatile vio_avail_t *avail;
    volatile vio_used_t *used;
    uint16_t last_used;
    vio_blk_req_t req;
    uint8_t status;
} vio_dev_t;

static uint8_t g_vio_rings[FBLK_MAX][VIO_RING_BYTES]
    __attribute__((aligned(4096)));
static vio_dev_t g_vio[FBLK_MAX];
static int g_vio_count;
static int g_vio_ready;
static volatile int g_vio_init_lock;
static volatile int g_vio_io_lock;

static uint32_t vio_rd(volatile uint32_t *base, uint32_t off)
{
    return base[off / 4u];
}

static void vio_wr(volatile uint32_t *base, uint32_t off, uint32_t value)
{
    base[off / 4u] = value;
}

static void vio_mb(void)
{
    __asm__ volatile("dsb sy" : : : "memory");
}

static void vio_rmb(void)
{
    __asm__ volatile("dmb ish" : : : "memory");
}

static int vio_probe(uint32_t slot)
{
    volatile uint32_t *mmio =
        (volatile uint32_t *)(uintptr_t)(VIO_MMIO_BASE +
                                         (uint64_t)slot * VIO_MMIO_STRIDE);
    if (vio_rd(mmio, VIO_MAGIC) != VIO_MAGIC_VALUE) {
        return 0;
    }
    uint32_t version = vio_rd(mmio, VIO_VERSION);
    if (version != 1u && version != 2u) {
        return 0;
    }
    if (vio_rd(mmio, VIO_DEVICE_ID) != VIO_DEVICE_BLOCK) {
        return 0;
    }
    if (vio_rd(mmio, VIO_VENDOR_ID) != VIO_VENDOR_QEMU) {
        return 0;
    }

    vio_dev_t *d = &g_vio[g_vio_count];
    finte_memset(d, 0, sizeof(*d));
    d->mmio = mmio;
    d->slot = slot;
    d->version = version;
    d->desc = (vio_desc_t *)(void *)(g_vio_rings[g_vio_count] + VIO_DESC_OFF);
    d->avail = (volatile vio_avail_t *)(void *)(g_vio_rings[g_vio_count] +
                                                VIO_AVAIL_OFF);
    if (version == 2u) {
        d->used = (volatile vio_used_t *)(void *)(g_vio_rings[g_vio_count] +
                                                  VIO_USED_OFF);
    } else {
        d->used = (volatile vio_used_t *)(void *)(g_vio_rings[g_vio_count] +
                                                  VIO_USED_LEGACY_OFF);
    }

    /* Reset the transport and wait for it to settle. */
    vio_wr(mmio, VIO_STATUS, 0);
    for (volatile uint32_t spin = 0;
         spin < 100000u && vio_rd(mmio, VIO_STATUS) != 0; spin++) {
    }
    vio_wr(mmio, VIO_STATUS, VIO_STATUS_ACK);
    vio_wr(mmio, VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER);

    vio_wr(mmio, VIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = vio_rd(mmio, VIO_DEVICE_FEATURES);
    uint32_t features_hi = 0;
    if (version == 2u) {
        vio_wr(mmio, VIO_DEVICE_FEATURES_SEL, 1);
        features_hi = vio_rd(mmio, VIO_DEVICE_FEATURES);
        if ((features_hi & 1u) == 0) { /* VIRTIO_F_VERSION_1 */
            return 0;
        }
    }
    d->readonly = (features_lo & (1u << VIRTIO_BLK_F_RO)) != 0;

    /* No optional features: the minimal driver needs no indirect
     * descriptors, event index or block-size negotiation. */
    vio_wr(mmio, VIO_DRIVER_FEATURES_SEL, 0);
    vio_wr(mmio, VIO_DRIVER_FEATURES, 0);
    if (version == 2u) {
        vio_wr(mmio, VIO_DRIVER_FEATURES_SEL, 1);
        vio_wr(mmio, VIO_DRIVER_FEATURES, 1);
        vio_wr(mmio, VIO_STATUS,
               VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_FEATURES_OK);
        if ((vio_rd(mmio, VIO_STATUS) & VIO_STATUS_FEATURES_OK) == 0) {
            return 0;
        }
    }

    vio_wr(mmio, VIO_QUEUE_SEL, 0);
    if (vio_rd(mmio, VIO_QUEUE_NUM_MAX) < VIO_QUEUE_SIZE) {
        return 0;
    }
    vio_wr(mmio, VIO_QUEUE_NUM, VIO_QUEUE_SIZE);

    uint64_t ring_pa = (uint64_t)(uintptr_t)g_vio_rings[g_vio_count];
    if (version == 2u) {
        uint64_t avail_pa = (uint64_t)(uintptr_t)d->avail;
        uint64_t used_pa = (uint64_t)(uintptr_t)d->used;
        vio_wr(mmio, VIO_QUEUE_DESC_LOW, (uint32_t)ring_pa);
        vio_wr(mmio, VIO_QUEUE_DESC_HIGH, (uint32_t)(ring_pa >> 32));
        vio_wr(mmio, VIO_QUEUE_DRIVER_LOW, (uint32_t)avail_pa);
        vio_wr(mmio, VIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail_pa >> 32));
        vio_wr(mmio, VIO_QUEUE_DEVICE_LOW, (uint32_t)used_pa);
        vio_wr(mmio, VIO_QUEUE_DEVICE_HIGH, (uint32_t)(used_pa >> 32));
        vio_wr(mmio, VIO_QUEUE_READY, 1);
        vio_wr(mmio, VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER |
                                     VIO_STATUS_FEATURES_OK |
                                     VIO_STATUS_DRIVER_OK);
    } else {
        vio_wr(mmio, VIO_GUEST_PAGE_SIZE, 4096);
        vio_wr(mmio, VIO_QUEUE_ALIGN, 4096);
        vio_wr(mmio, VIO_QUEUE_PFN, (uint32_t)(ring_pa >> 12));
        vio_wr(mmio, VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER |
                                     VIO_STATUS_DRIVER_OK);
    }

    d->capacity = (uint64_t)vio_rd(mmio, VIO_CONFIG) |
                  ((uint64_t)vio_rd(mmio, VIO_CONFIG + 4u) << 32);
    if (d->capacity == 0) {
        return 0;
    }
    d->last_used = 0;
    g_vio_count++;
    return 1;
}

static void vio_init(void)
{
    if (__atomic_load_n(&g_vio_ready, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_vio_init_lock);
    if (!g_vio_ready) {
        g_vio_count = 0;
        for (uint32_t i = 0; i < VIO_MMIO_SLOTS; i++) {
            (void)vio_probe(i);
        }
        __atomic_store_n(&g_vio_ready, 1, __ATOMIC_RELEASE);
    }
    hal_spin_unlock(&g_vio_init_lock);
    hal_irq_restore(flags);
}

static int vio_request(vio_dev_t *d, uint32_t type, uint64_t lba,
                       void *buf, uint32_t sectors)
{
    if (!d || !buf || sectors == 0 || sectors > VIO_MAX_SECTORS ||
        lba > d->capacity || sectors > d->capacity - lba) {
        return -1;
    }
    if (type == VIO_REQ_OUT && d->readonly) {
        return -1;
    }

    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_vio_io_lock);

    d->req.type = type;
    d->req.reserved = 0;
    d->req.sector = lba;
    d->status = 0xFF;

    d->desc[0].addr = (uint64_t)(uintptr_t)&d->req;
    d->desc[0].len = sizeof(d->req);
    d->desc[0].flags = 1u; /* NEXT */
    d->desc[0].next = 1;
    d->desc[1].addr = (uint64_t)(uintptr_t)buf;
    d->desc[1].len = sectors * 512u;
    d->desc[1].flags = 1u | (type == VIO_REQ_IN ? 2u : 0u); /* NEXT|WRITE */
    d->desc[1].next = 2;
    d->desc[2].addr = (uint64_t)(uintptr_t)&d->status;
    d->desc[2].len = 1;
    d->desc[2].flags = 2u; /* WRITE */
    d->desc[2].next = 0;

    d->avail->ring[d->avail->idx % VIO_QUEUE_SIZE] = 0;
    vio_mb();
    d->avail->idx++;
    vio_mb();
    vio_wr(d->mmio, VIO_QUEUE_NOTIFY, 0);

    uint64_t timeout = hal_cycles() + hal_cycles_per_sec() * 2u;
    while (d->used->idx == d->last_used) {
        if (hal_cycles() > timeout) {
            hal_spin_unlock(&g_vio_io_lock);
            hal_irq_restore(flags);
            return -1;
        }
        __asm__ volatile("yield");
    }
    vio_rmb();
    uint32_t id = d->used->ring[d->last_used % VIO_QUEUE_SIZE].id;
    d->last_used++;
    int ok = (id == 0 && d->status == 0);

    hal_spin_unlock(&g_vio_io_lock);
    hal_irq_restore(flags);
    return ok ? 0 : -1;
}

int hal_blk_count(void)
{
    vio_init();
    return g_vio_count;
}

int hal_blk_info(int index, fblk_info_t *out)
{
    vio_init();
    if (!out || index < 0 || index >= g_vio_count) {
        return -1;
    }
    const vio_dev_t *d = &g_vio[index];
    finte_memset(out, 0, sizeof(*out));
    finte_format(out->name, sizeof(out->name), "virtio%d", index);
    out->sectors = d->capacity;
    out->sector_size = 512;
    out->flags = 0;
    return 0;
}

int hal_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors)
{
    vio_init();
    if (index < 0 || index >= g_vio_count) {
        return -1;
    }
    return vio_request(&g_vio[index], VIO_REQ_IN, lba, buf, sectors);
}

int hal_blk_write(int index, uint64_t lba, const void *buf, uint32_t sectors)
{
    vio_init();
    if (index < 0 || index >= g_vio_count) {
        return -1;
    }
    return vio_request(&g_vio[index], VIO_REQ_OUT, lba, (void *)buf, sectors);
}
