/* xHCI controller, boot HID keyboard and USB mass storage (BOT).
 *
 * Polled only: the controller event ring is drained from shell input and
 * block I/O, so no MSI/MSI-X routing is required. */
#include "finte/finte.h"
#include "xhci.h"
#include "input.h"
#include "hal_x86.h"

#define XHCI_MAX_CTRL 2
#define XHCI_MAX_DEV 8
#define XHCI_RING_TRBS 256

#define XHCI_CMD_ENABLE_SLOT 0x09
#define XHCI_CMD_ADDRESS_DEV 0x0B
#define XHCI_CMD_CONFIGURE_EP 0x0C

#define XHCI_TRB_NORMAL 1
#define XHCI_TRB_SETUP 2
#define XHCI_TRB_DATA 3
#define XHCI_TRB_STATUS 4
#define XHCI_TRB_LINK 6
#define XHCI_TRB_EVT_XFER 32
#define XHCI_TRB_EVT_CMD 33
#define XHCI_TRB_EVT_PORT 34

#define XHCI_CC_SUCCESS 1

typedef struct xtrb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xtrb_t;

typedef struct xhci_ctrl {
    int used;
    volatile uint32_t *mmio;
    uint32_t op;
    uint32_t db;
    uint32_t rt;
    uint32_t max_slots;
    uint32_t max_ports;
    xtrb_t *cmd;
    xtrb_t *evt;
    uint64_t *dcbaa;
    uint8_t *input;
    uint32_t cmd_tail;
    uint32_t evt_head;
    uint16_t cmd_cycle;
    uint16_t evt_cycle;
    uint64_t evt_pa;
} xhci_ctrl_t;

typedef struct xhci_dev {
    int used;
    xhci_ctrl_t *ctrl;
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    uint8_t *dev_ctx;
    uint16_t ep0_mps;
    xtrb_t *ep0;
    uint32_t ep0_tail;
    uint16_t ep0_cycle;
    int hid;
    uint8_t hid_ep;
    uint16_t hid_mps;
    uint8_t hid_interval;
    xtrb_t *hid_ring;
    uint32_t hid_tail;
    uint16_t hid_cycle;
    uint8_t hid_buf[8] __attribute__((aligned(64)));
    uint8_t hid_prev[6];
    uint16_t keyq[32];
    uint8_t key_head;
    uint8_t key_count;
    int storage;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    xtrb_t *bulk_in_ring;
    xtrb_t *bulk_out_ring;
    uint32_t bulk_in_tail;
    uint32_t bulk_out_tail;
    uint16_t bulk_in_cycle;
    uint16_t bulk_out_cycle;
    uint8_t *bot;
    uint8_t *io_buf;
    uint32_t bot_tag;
    uint64_t sectors;
    char name[FBLK_NAME_MAX];
} xhci_dev_t;

static xhci_ctrl_t g_ctrl[XHCI_MAX_CTRL];
static xhci_dev_t g_dev[XHCI_MAX_DEV];
static uint8_t g_desc[4096] __attribute__((aligned(64)));
static int g_xhci_ready;
static int xhci_alloc_dev_index;
static volatile int g_xhci_io_lock;

static inline uint32_t rd32(volatile uint32_t *p, uint32_t off)
{
    return p[off / 4u];
}

static inline void wr32(volatile uint32_t *p, uint32_t off, uint32_t v)
{
    p[off / 4u] = v;
}

static inline uint64_t rd64(volatile uint32_t *p, uint32_t off)
{
    uint64_t lo = rd32(p, off);
    uint64_t hi = rd32(p, off + 4u);
    return lo | (hi << 32);
}

static inline void wr64(volatile uint32_t *p, uint32_t off, uint64_t v)
{
    wr32(p, off, (uint32_t)v);
    wr32(p, off + 4u, (uint32_t)(v >> 32));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
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

static uint32_t pci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_wr16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                     uint16_t value)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (off & 0xFC);
    outl(0xCF8, addr);
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"((uint16_t)0xCFC));
}

static uint64_t deadline_ns(uint64_t ms)
{
    return hal_now_ns() + ms * 1000000ull;
}

static int wait_bits(volatile uint32_t *p, uint32_t off, uint32_t mask,
                     uint32_t value, uint64_t ms)
{
    uint64_t end = deadline_ns(ms);
    while (hal_now_ns() < end) {
        uint32_t v = rd32(p, off);
        if ((v & mask) == value) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static void ring_link(xtrb_t *ring)
{
    finte_memset(&ring[XHCI_RING_TRBS - 1], 0, sizeof(xtrb_t));
    ring[XHCI_RING_TRBS - 1].param = (uint64_t)(uintptr_t)ring;
    ring[XHCI_RING_TRBS - 1].control =
        (XHCI_TRB_LINK << 10) | (1u << 1) | 1u;
}

static int ctrl_event(xhci_ctrl_t *c, uint32_t *type, uint32_t *slot,
                      uint32_t *ep, uint32_t *code)
{
    xtrb_t *e = &c->evt[c->evt_head];
    if ((e->control & 1u) != c->evt_cycle) {
        return 0;
    }
    *type = (e->control >> 10) & 0x3Fu;
    *ep = (e->control >> 16) & 0x1Fu;
    *slot = (e->control >> 24) & 0xFFu;
    *code = (e->status >> 24) & 0xFFu;
    c->evt_head = (c->evt_head + 1u) % XHCI_RING_TRBS;
    if (c->evt_head == 0) {
        c->evt_cycle ^= 1u;
    }
    wr64(c->mmio, c->rt + 0x18,
         c->evt_pa + (uint64_t)c->evt_head * 16u + 8u);
    return 1;
}

static int ctrl_command(xhci_ctrl_t *c, uint32_t op, uint64_t param,
                        uint32_t slot, uint32_t *out_slot)
{
    xtrb_t *t = &c->cmd[c->cmd_tail];
    finte_memset(t, 0, sizeof(*t));
    t->param = param;
    t->control = (op << 10) | ((slot & 0xFFu) << 24) | c->cmd_cycle;
    c->cmd_tail = (c->cmd_tail + 1u) % XHCI_RING_TRBS;
    if (c->cmd_tail == 0) {
        c->cmd_cycle ^= 1u;
    }
    __asm__ volatile("mfence" : : : "memory");
    wr32(c->mmio, c->db, 0u);

    uint64_t end = deadline_ns(2000);
    while (hal_now_ns() < end) {
        uint32_t type, eslot, ep, code;
        if (!ctrl_event(c, &type, &eslot, &ep, &code)) {
            __asm__ volatile("pause");
            continue;
        }
        if (type == XHCI_TRB_EVT_CMD) {
            if (out_slot) {
                *out_slot = eslot;
            }
            return code == XHCI_CC_SUCCESS ? 0 : -1;
        }
    }
    return -1;
}

static int port_reset(xhci_ctrl_t *c, uint32_t port, uint8_t *speed)
{
    uint32_t base = c->op + 0x400u + (port - 1u) * 0x10u;
    uint32_t p = rd32(c->mmio, base);
    if ((p & 1u) == 0) {
        return -1;
    }
    wr32(c->mmio, base, p | (1u << 9));
    wr32(c->mmio, base, (p & ~0xFE0000u) | (1u << 4));
    uint64_t end = deadline_ns(500);
    for (;;) {
        p = rd32(c->mmio, base);
        if ((p & (1u << 21)) != 0 || (p & (1u << 4)) == 0) {
            break;
        }
        if (hal_now_ns() >= end) {
            return -1;
        }
        __asm__ volatile("pause");
    }
    if ((p & 1u) == 0) {
        return -1;
    }
    wr32(c->mmio, base, p | (1u << 21) | (1u << 17) | (1u << 18) |
                              (1u << 19) | (1u << 20) | (1u << 22) |
                              (1u << 23));
    *speed = (uint8_t)((p >> 10) & 0xFu);
    return 0;
}

static uint32_t *input_slot(xhci_ctrl_t *c)
{
    return (uint32_t *)(void *)(c->input + 32u);
}

static uint32_t *input_ep(xhci_ctrl_t *c, uint32_t ep_id)
{
    return (uint32_t *)(void *)(c->input + 64u + (ep_id - 1u) * 32u);
}

static void fill_ep0(xhci_ctrl_t *c, xhci_dev_t *d)
{
    uint32_t *ep = input_ep(c, 1);
    finte_memset(ep, 0, 32);
    ep[1] = ((uint32_t)d->ep0_mps << 16) | (3u << 1) | (4u << 3);
    ep[2] = (uint32_t)(uintptr_t)d->ep0 | 1u;
    ep[3] = (uint32_t)((uint64_t)(uintptr_t)d->ep0 >> 32);
    ep[4] = 8;
}

static int dev_address(xhci_ctrl_t *c, xhci_dev_t *d)
{
    finte_memset(c->input, 0, 4096);
    uint32_t *icc = (uint32_t *)(void *)c->input;
    icc[1] = (1u << 0) | (1u << 1);
    uint32_t *slot = input_slot(c);
    slot[0] = ((uint32_t)d->speed << 20) | (1u << 27);
    slot[1] = (uint32_t)d->port << 16;
    fill_ep0(c, d);
    uint32_t got = 0;
    int rc = ctrl_command(c, XHCI_CMD_ADDRESS_DEV,
                          (uint64_t)(uintptr_t)c->input, d->slot, &got);
    return rc;
}

static int control_xfer(xhci_dev_t *d, uint8_t bm, uint8_t req,
                        uint16_t value, uint16_t index, void *buf,
                        uint16_t len, int in)
{
    xhci_ctrl_t *c = d->ctrl;
    xtrb_t *t = &d->ep0[d->ep0_tail];
    uint16_t cycle = d->ep0_cycle;
    finte_memset(t, 0, 3 * sizeof(xtrb_t));
    uint64_t setup = (uint64_t)bm | ((uint64_t)req << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)len << 48);
    t[0].param = setup;
    t[0].status = 8;
    t[0].control = (XHCI_TRB_SETUP << 10) | (1u << 6) | (1u << 4) | cycle;
    uint32_t n = 1;
    if (len) {
        t[n].param = (uint64_t)(uintptr_t)buf;
        t[n].status = len;
        t[n].control = (XHCI_TRB_DATA << 10) | (1u << 4) |
                       (in ? (1u << 16) : 0u) | (1u << 5) | cycle;
        n++;
    }
    t[n].control = (XHCI_TRB_STATUS << 10) |
                   (in ? 0u : (1u << 16)) | (1u << 5) | cycle;
    d->ep0_tail = (d->ep0_tail + n + 1u) % XHCI_RING_TRBS;
    wr32(c->mmio, c->db + (uint32_t)d->slot * 4u, 1u);

    uint64_t end = deadline_ns(2000);
    while (hal_now_ns() < end) {
        uint32_t type, slot, ep, code;
        if (!ctrl_event(c, &type, &slot, &ep, &code)) {
            __asm__ volatile("pause");
            continue;
        }
        if (type == XHCI_TRB_EVT_XFER && slot == d->slot && ep == 1u) {
            return code == XHCI_CC_SUCCESS ? 0 : -1;
        }
    }
    return -1;
}

static uint16_t rd16p(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int set_config(xhci_dev_t *d, uint8_t cfg)
{
    return control_xfer(d, 0x00, 9, cfg, 0, NULL, 0, 0);
}

static int configure_ep(xhci_ctrl_t *c, xhci_dev_t *d, uint32_t ep_id,
                        uint16_t mps, uint8_t type, uint8_t interval,
                        xtrb_t *ring, uint32_t *tail, uint16_t cycle)
{
    (void)tail;
    (void)cycle;
    finte_memset(c->input, 0, 4096);
    uint32_t *icc = (uint32_t *)(void *)c->input;
    icc[1] = (1u << 0) | (1u << ep_id);
    uint32_t *slot = input_slot(c);
    slot[0] = ((uint32_t)d->speed << 20) | (ep_id << 27);
    slot[1] = (uint32_t)d->port << 16;
    slot[3] = d->slot;

    uint32_t *ep = input_ep(c, ep_id);
    ep[0] = (uint32_t)interval << 16;
    ep[1] = ((uint32_t)mps << 16) | (3u << 1) | ((uint32_t)type << 3);
    ep[2] = (uint32_t)(uintptr_t)ring | 1u;
    ep[3] = (uint32_t)((uint64_t)(uintptr_t)ring >> 32);
    ep[4] = 8;
    uint32_t got = 0;
    return ctrl_command(c, XHCI_CMD_CONFIGURE_EP,
                        (uint64_t)(uintptr_t)c->input, d->slot, &got);
}

static void hid_queue(xhci_dev_t *d)
{
    xhci_ctrl_t *c = d->ctrl;
    xtrb_t *t = &d->hid_ring[d->hid_tail];
    t->param = (uint64_t)(uintptr_t)d->hid_buf;
    t->status = 8;
    t->control = (XHCI_TRB_NORMAL << 10) | (1u << 5) | d->hid_cycle;
    d->hid_tail = (d->hid_tail + 1u) % XHCI_RING_TRBS;
    if (d->hid_tail == 0) {
        d->hid_cycle ^= 1u;
    }
    wr32(c->mmio, c->db + (uint32_t)d->slot * 4u, d->hid_ep);
}

static int bulk_xfer(xhci_dev_t *d, uint8_t ep_id, void *buf, uint32_t len,
                     int in)
{
    xtrb_t *ring = in ? d->bulk_in_ring : d->bulk_out_ring;
    uint32_t *tail = in ? &d->bulk_in_tail : &d->bulk_out_tail;
    uint16_t *cycle = in ? &d->bulk_in_cycle : &d->bulk_out_cycle;
    xhci_ctrl_t *c = d->ctrl;
    if (!ring || len == 0) {
        return -1;
    }
    xtrb_t *t = &ring[*tail];
    t->param = (uint64_t)(uintptr_t)buf;
    t->status = len;
    t->control = (XHCI_TRB_NORMAL << 10) | (1u << 5) | *cycle;
    *tail = (*tail + 1u) % XHCI_RING_TRBS;
    if (*tail == 0) {
        *cycle ^= 1u;
    }
    __asm__ volatile("mfence" : : : "memory");
    wr32(c->mmio, c->db + (uint32_t)d->slot * 4u, ep_id);

    uint64_t end = deadline_ns(3000);
    while (hal_now_ns() < end) {
        uint32_t type, slot, ep, code;
        if (!ctrl_event(c, &type, &slot, &ep, &code)) {
            __asm__ volatile("pause");
            continue;
        }
        if (type == XHCI_TRB_EVT_XFER && slot == d->slot && ep == ep_id) {
            return (code == XHCI_CC_SUCCESS || code == 13) ? 0 : -1;
        }
    }
    return -1;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int bot_command(xhci_dev_t *d, const uint8_t *cdb, uint8_t cdb_len,
                       uint32_t data_len, int data_in, void *data)
{
    uint8_t *cbw = d->bot;
    uint8_t *csw = d->bot + 64;
    finte_memset(cbw, 0, 31);
    put_le32(cbw + 0, 0x43425355u);
    put_le32(cbw + 4, d->bot_tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = (data_len != 0 && data_in) ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdb_len;
    finte_memcpy(cbw + 15, cdb, cdb_len);
    if (bulk_xfer(d, d->bulk_out, cbw, 31, 0) != 0) {
        return -1;
    }
    if (data_len && bulk_xfer(d, data_in ? d->bulk_in : d->bulk_out, data,
                              data_len, data_in) != 0) {
        return -1;
    }
    finte_memset(csw, 0, 13);
    if (bulk_xfer(d, d->bulk_in, csw, 13, 1) != 0) {
        return -1;
    }
    uint32_t sig = (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) |
                   ((uint32_t)csw[2] << 16) | ((uint32_t)csw[3] << 24);
    uint32_t tag = (uint32_t)csw[4] | ((uint32_t)csw[5] << 8) |
                   ((uint32_t)csw[6] << 16) | ((uint32_t)csw[7] << 24);
    if (sig != 0x53425355u || tag != d->bot_tag || csw[12] != 0) {
        return -1;
    }
    d->bot_tag++;
    return 0;
}

static int configure_pair(xhci_ctrl_t *c, xhci_dev_t *d, uint8_t out_ep,
                          uint16_t out_mps, xtrb_t *out_ring,
                          uint8_t in_ep, uint16_t in_mps, xtrb_t *in_ring)
{
    finte_memset(c->input, 0, 4096);
    uint32_t *icc = (uint32_t *)(void *)c->input;
    icc[1] = (1u << 0) | (1u << out_ep) | (1u << in_ep);
    uint32_t max_ep = out_ep > in_ep ? out_ep : in_ep;
    uint32_t *slot = input_slot(c);
    slot[0] = ((uint32_t)d->speed << 20) | (max_ep << 27);
    slot[1] = (uint32_t)d->port << 16;
    slot[3] = d->slot;

    uint32_t *ep = input_ep(c, out_ep);
    ep[1] = ((uint32_t)out_mps << 16) | (3u << 1) | (2u << 3);
    ep[2] = (uint32_t)(uintptr_t)out_ring | 1u;
    ep[3] = (uint32_t)((uint64_t)(uintptr_t)out_ring >> 32);
    ep[4] = 8;
    ep = input_ep(c, in_ep);
    ep[1] = ((uint32_t)in_mps << 16) | (3u << 1) | (6u << 3);
    ep[2] = (uint32_t)(uintptr_t)in_ring | 1u;
    ep[3] = (uint32_t)((uint64_t)(uintptr_t)in_ring >> 32);
    ep[4] = 8;
    uint32_t got = 0;
    return ctrl_command(c, XHCI_CMD_CONFIGURE_EP,
                        (uint64_t)(uintptr_t)c->input, d->slot, &got);
}

static int storage_init(xhci_dev_t *d, uint8_t in_ep, uint16_t in_mps,
                        uint8_t out_ep, uint16_t out_mps)
{
    d->bulk_in = (uint8_t)((in_ep << 1) | 1);
    d->bulk_out = (uint8_t)(out_ep << 1);
    d->bulk_in_mps = in_mps;
    d->bulk_out_mps = out_mps;
    d->bulk_in_ring = (xtrb_t *)hal_pages_alloc(4096);
    d->bulk_out_ring = (xtrb_t *)hal_pages_alloc(4096);
    d->bot = (uint8_t *)hal_pages_alloc(4096);
    d->io_buf = (uint8_t *)hal_pages_alloc(4096);
    if (!d->bulk_in_ring || !d->bulk_out_ring || !d->bot || !d->io_buf) {
        return -1;
    }
    ring_link(d->bulk_in_ring);
    ring_link(d->bulk_out_ring);
    d->bulk_in_cycle = 1;
    d->bulk_out_cycle = 1;
    if (configure_pair(d->ctrl, d, d->bulk_out, out_mps, d->bulk_out_ring,
                       d->bulk_in, in_mps, d->bulk_in_ring) != 0) {
        return -1;
    }

    uint8_t cdb[16];
    d->bot_tag = 1;
    finte_memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25; /* READ CAPACITY(10) */
    finte_memset(d->io_buf, 0, 4096);
    if (bot_command(d, cdb, 10, 8, 1, d->io_buf) != 0) {
        return -1;
    }
    uint32_t last = get_be32(d->io_buf);
    uint32_t bsize = get_be32(d->io_buf + 4);
    if (bsize != 512u || last == 0xFFFFFFFFu) {
        return -1;
    }
    d->sectors = (uint64_t)last + 1u;
    d->storage = 1;
    return 0;
}

static int storage_read(xhci_dev_t *d, uint64_t lba, void *buf,
                        uint32_t sectors)
{
    uint32_t done = 0;
    while (done < sectors) {
        uint32_t chunk = sectors - done;
        if (chunk > 8u) {
            chunk = 8u;
        }
        uint8_t cdb[16];
        finte_memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x28; /* READ(10) */
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;
        if (bot_command(d, cdb, 10, chunk * 512u, 1, d->io_buf) != 0) {
            return -1;
        }
        finte_memcpy((uint8_t *)buf + (size_t)done * 512u, d->io_buf,
                     (size_t)chunk * 512u);
        done += chunk;
        lba += chunk;
    }
    return 0;
}

static int storage_write(xhci_dev_t *d, uint64_t lba, const void *buf,
                         uint32_t sectors)
{
    uint32_t done = 0;
    while (done < sectors) {
        uint32_t chunk = sectors - done;
        if (chunk > 8u) {
            chunk = 8u;
        }
        finte_memcpy(d->io_buf, (const uint8_t *)buf + (size_t)done * 512u,
                     (size_t)chunk * 512u);
        uint8_t cdb[16];
        finte_memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x2A; /* WRITE(10) */
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;
        if (bot_command(d, cdb, 10, chunk * 512u, 0, d->io_buf) != 0) {
            return -1;
        }
        done += chunk;
        lba += chunk;
    }
    return 0;
}

static void key_push(xhci_dev_t *d, uint16_t ch)
{
    if (d->key_count >= sizeof(d->keyq) / sizeof(d->keyq[0])) {
        return;
    }
    uint8_t tail = (uint8_t)((d->key_head + d->key_count) %
                             (sizeof(d->keyq) / sizeof(d->keyq[0])));
    d->keyq[tail] = ch;
    d->key_count++;
}

static uint16_t hid_ascii(uint8_t code, int shift)
{
    if (code >= 0x04 && code <= 0x1D) {
        char c = (char)('a' + (code - 0x04));
        return (uint16_t)(shift ? (c - 'a' + 'A') : c);
    }
    if (code >= 0x1E && code <= 0x27) {
        static const char digits[] = "1234567890";
        char c = digits[code - 0x1E];
        if (!shift) {
            return (uint16_t)c;
        }
        static const char sym[] = "!@#$%^&*()";
        return (uint16_t)sym[code - 0x1E];
    }
    switch (code) {
    case 0x28:
        return '\n';
    case 0x2A:
        return '\b';
    case 0x2B:
        return '\t';
    case 0x2C:
        return ' ';
    case 0x2D:
        return shift ? '_' : '-';
    case 0x2E:
        return shift ? '+' : '=';
    case 0x2F:
        return shift ? '{' : '[';
    case 0x30:
        return shift ? '}' : ']';
    case 0x31:
        return shift ? '|' : '\\';
    case 0x33:
        return shift ? ':' : ';';
    case 0x34:
        return shift ? '"' : '\'';
    case 0x35:
        return shift ? '~' : '`';
    case 0x36:
        return shift ? '<' : ',';
    case 0x37:
        return shift ? '>' : '.';
    case 0x38:
        return shift ? '?' : '/';
    case 0x4A:
        return KEY_HOME;
    case 0x4B:
        return KEY_PGUP;
    case 0x4C:
        return KEY_DELETE;
    case 0x4D:
        return KEY_END;
    case 0x4E:
        return KEY_PGDN;
    case 0x4F:
        return KEY_RIGHT;
    case 0x50:
        return KEY_LEFT;
    case 0x51:
        return KEY_DOWN;
    case 0x52:
        return KEY_UP;
    default:
        return 0;
    }
}

static void hid_report(xhci_dev_t *d)
{
    int shift = (d->hid_buf[0] & 0x22u) != 0;
    for (int i = 0; i < 6; i++) {
        uint8_t code = d->hid_buf[2 + i];
        int seen = 0;
        for (int j = 0; j < 6; j++) {
            if (d->hid_prev[j] == code && code) {
                seen = 1;
                break;
            }
        }
        if (code && !seen) {
            uint16_t ch = hid_ascii(code, shift);
            if (ch) {
                key_push(d, ch);
            }
        }
        d->hid_prev[i] = code;
    }
}

static int hid_poll_events(void)
{
    for (int ci = 0; ci < XHCI_MAX_CTRL; ci++) {
        xhci_ctrl_t *c = &g_ctrl[ci];
        if (!c->used) {
            continue;
        }
        for (;;) {
            uint32_t type, slot, ep, code;
            if (!ctrl_event(c, &type, &slot, &ep, &code)) {
                break;
            }
            if (type != XHCI_TRB_EVT_XFER) {
                continue;
            }
            for (int di = 0; di < XHCI_MAX_DEV; di++) {
                xhci_dev_t *d = &g_dev[di];
                if (!d->used || d->ctrl != c || d->slot != slot ||
                    !d->hid || ep != d->hid_ep) {
                    continue;
                }
                if (code == XHCI_CC_SUCCESS) {
                    hid_report(d);
                }
                hid_queue(d);
            }
        }
    }
    return 0;
}

int xhci_trykey(void)
{
    if (!g_xhci_ready) {
        return -1;
    }
    (void)hid_poll_events();
    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        xhci_dev_t *d = &g_dev[i];
        if (d->used && d->hid && d->key_count) {
            uint16_t ch = d->keyq[d->key_head];
            d->key_head = (uint8_t)((d->key_head + 1u) %
                                    (sizeof(d->keyq) /
                                     sizeof(d->keyq[0])));
            d->key_count--;
            return ch;
        }
    }
    return -1;
}

static void dev_enumerate(xhci_ctrl_t *c, uint8_t slot, uint8_t port,
                          uint8_t speed)
{
    if (xhci_alloc_dev_index < 0) {
        return;
    }
    xhci_dev_t *d = &g_dev[xhci_alloc_dev_index];
    finte_memset(d, 0, sizeof(*d));
    d->ctrl = c;
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->ep0_mps = speed == 4 ? 512 : (speed == 3 ? 64 : 8);
    d->dev_ctx = (uint8_t *)hal_pages_alloc(4096);
    if (!d->dev_ctx) {
        return;
    }
    finte_memset(d->dev_ctx, 0, 4096);
    c->dcbaa[slot] = (uint64_t)(uintptr_t)d->dev_ctx;
    d->ep0 = (xtrb_t *)hal_pages_alloc(4096);
    if (!d->ep0) {
        return;
    }
    ring_link(d->ep0);
    d->ep0_cycle = 1;

    if (dev_address(c, d) != 0) {
        return;
    }
    finte_memset(g_desc, 0, sizeof(g_desc));
    if (control_xfer(d, 0x80, 6, 0x0100, 0, g_desc, 8, 1) != 0) {
        return;
    }
    if (control_xfer(d, 0x80, 6, 0x0100, 0, g_desc, 18, 1) != 0) {
        return;
    }
    uint16_t mps = g_desc[7];
    if (mps && mps != d->ep0_mps) {
        d->ep0_mps = mps;
    }
    finte_memset(g_desc, 0, sizeof(g_desc));
    if (control_xfer(d, 0x80, 6, 0x0200, 0, g_desc, 255, 1) != 0) {
        return;
    }
    uint16_t total = rd16p(g_desc + 2);
    if (total < 9 || total > sizeof(g_desc)) {
        return;
    }
    uint8_t cfg = g_desc[5];
    if (set_config(d, cfg) != 0) {
        return;
    }

    int cur_class = -1, cur_sub = -1, cur_proto = -1;
    int hid_ep = -1;
    uint16_t hid_mps = 0;
    uint8_t hid_interval = 0;
    int st_in = -1;
    int st_out = -1;
    uint16_t st_in_mps = 0;
    uint16_t st_out_mps = 0;
    for (uint32_t i = 0; i + 2u < total;) {
        uint8_t len = g_desc[i];
        uint8_t type = g_desc[i + 1];
        if (len < 2 || i + len > total) {
            break;
        }
        if (type == 4 && len >= 9) {
            cur_class = g_desc[i + 5];
            cur_sub = g_desc[i + 6];
            cur_proto = g_desc[i + 7];
        } else if (type == 5 && len >= 7) {
            uint8_t ep = g_desc[i + 2];
            uint8_t attr = g_desc[i + 3];
            uint16_t m = (uint16_t)(rd16p(g_desc + i + 4) & 0x7FFu);
            uint8_t iv = g_desc[i + 6];
            if (cur_class == 3 && cur_sub == 1 && cur_proto == 1 &&
                (ep & 0x80) && (attr & 3u) == 3u) {
                hid_ep = (int)(ep & 0x0F);
                hid_mps = m;
                hid_interval = iv;
            }
            if (cur_class == 8 && cur_sub == 6 && cur_proto == 0x50 &&
                (attr & 3u) == 2u) {
                if (ep & 0x80) {
                    st_in = (int)(ep & 0x0F);
                    st_in_mps = m;
                } else {
                    st_out = (int)(ep & 0x0F);
                    st_out_mps = m;
                }
            }
        }
        i += len;
    }

    if (hid_ep > 0) {
        uint8_t ep_id = (uint8_t)((hid_ep << 1) | 1);
        d->hid_ring = (xtrb_t *)hal_pages_alloc(4096);
        if (!d->hid_ring) {
            return;
        }
        ring_link(d->hid_ring);
        d->hid_cycle = 1;
        if (configure_ep(c, d, ep_id, hid_mps, 7, hid_interval,
                         d->hid_ring, &d->hid_tail, 1) != 0) {
            return;
        }
        d->hid = 1;
        d->hid_ep = ep_id;
        d->hid_mps = hid_mps;
        d->hid_interval = hid_interval;
        hid_queue(d);
    }
    if (st_in > 0 && st_out > 0) {
        (void)storage_init(d, (uint8_t)st_in, st_in_mps, (uint8_t)st_out,
                           st_out_mps);
    }
    finte_format(d->name, sizeof(d->name), "usb%d", xhci_alloc_dev_index);
    d->used = 1;
    xhci_alloc_dev_index++;
}

static int ctrl_start(xhci_ctrl_t *c, volatile uint32_t *mmio)
{
    finte_memset(c, 0, sizeof(*c));
    c->mmio = mmio;
    uint8_t cap_len = *(volatile uint8_t *)(void *)mmio;
    uint32_t hcs1 = rd32(mmio, 0x04);
    uint32_t hcs2 = rd32(mmio, 0x08);
    c->max_slots = hcs1 & 0xFFu;
    c->max_ports = (hcs1 >> 24) & 0xFFu;
    if (c->max_slots == 0 || c->max_ports == 0) {
        return -1;
    }
    c->op = cap_len;
    c->db = rd32(mmio, 0x14);
    c->rt = (rd32(mmio, 0x18) & ~0x1Fu) + 0x20u;

    uint32_t cc = rd32(mmio, c->op);
    if (cc & 1u) {
        wr32(mmio, c->op, cc & ~1u);
        if (wait_bits(mmio, c->op + 4u, 1u, 1u, 500) != 0) {
            return -1;
        }
    }
    wr32(mmio, c->op, 2u);
    if (wait_bits(mmio, c->op, 2u, 0, 1000) != 0 ||
        wait_bits(mmio, c->op + 4u, 1u, 1u, 1000) != 0) {
        return -1;
    }

    c->cmd = (xtrb_t *)hal_pages_alloc(4096);
    c->evt = (xtrb_t *)hal_pages_alloc(4096);
    c->dcbaa = (uint64_t *)hal_pages_alloc(4096);
    c->input = (uint8_t *)hal_pages_alloc(4096);
    uint8_t *erst = (uint8_t *)hal_pages_alloc(4096);
    if (!c->cmd || !c->evt || !c->dcbaa || !c->input || !erst) {
        return -1;
    }
    finte_memset(c->cmd, 0, 4096);
    finte_memset(c->evt, 0, 4096);
    finte_memset(c->dcbaa, 0, 4096);
    finte_memset(c->input, 0, 4096);
    finte_memset(erst, 0, 4096);
    ring_link(c->cmd);
    c->cmd_cycle = 1;
    c->evt_cycle = 1;
    c->evt_pa = (uint64_t)(uintptr_t)c->evt;
    ((uint64_t *)c->evt)[XHCI_RING_TRBS - 1] = 0;

    uint32_t scratch = ((hcs2 >> 21) & 0x1Fu) |
                       (((hcs2 >> 27) & 0x1Fu) << 5);
    if (scratch) {
        uint64_t *arr = (uint64_t *)hal_pages_alloc(4096);
        if (!arr) {
            return -1;
        }
        for (uint32_t i = 0; i < scratch && i < 64; i++) {
            void *page = hal_pages_alloc(4096);
            if (!page) {
                return -1;
            }
            arr[i] = (uint64_t)(uintptr_t)page;
        }
        c->dcbaa[0] = (uint64_t)(uintptr_t)arr;
    }

    *(uint64_t *)(void *)(erst + 0) = c->evt_pa;
    *(uint32_t *)(void *)(erst + 8) = XHCI_RING_TRBS;
    *(uint32_t *)(void *)(erst + 12) = 0;
    wr32(mmio, c->op + 0x38u, c->max_slots);
    wr64(mmio, c->op + 0x30u, (uint64_t)(uintptr_t)c->dcbaa);
    wr64(mmio, c->op + 0x18u, (uint64_t)(uintptr_t)c->cmd | 1u);
    wr32(mmio, c->rt + 0x08u, 1u);
    wr64(mmio, c->rt + 0x10u, (uint64_t)(uintptr_t)erst);
    wr64(mmio, c->rt + 0x18u, c->evt_pa | 8u);
    wr32(mmio, c->rt + 0x00u, 2u);
    wr32(mmio, c->rt + 0x04u, 0u);
    wr32(mmio, c->op + 0x04u, 0x1Cu);
    wr32(mmio, c->op, 1u);
    if (wait_bits(mmio, c->op + 4u, 1u, 0, 1000) != 0) {
        return -1;
    }
    hal_idle(50000000ull);
    c->used = 1;
    return 0;
}

static void xhci_scan(void)
{
    if (!hal_is_uefi()) {
        return;
    }
    int ctrl_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_rd32((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)fn, 0x00);
                if ((id & 0xFFFFu) == 0xFFFFu) {
                    continue;
                }
                uint32_t cls = pci_rd32((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)fn, 0x08);
                if (((cls >> 24) & 0xFFu) != 0x0Cu ||
                    ((cls >> 16) & 0xFFu) != 0x03u ||
                    ((cls >> 8) & 0xFFu) != 0x30u) {
                    continue;
                }
                if (ctrl_count >= XHCI_MAX_CTRL) {
                    return;
                }
                uint32_t raw = pci_rd32((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)fn, 0x10);
                if (raw & 1u) {
                    continue;
                }
                uint32_t type = (raw >> 1) & 3u;
                uint64_t bar = (uint64_t)(raw & ~0xFu);
                if (type == 2u) {
                    bar |= (uint64_t)pci_rd32((uint8_t)bus, (uint8_t)dev,
                                              (uint8_t)fn, 0x14) << 32;
                }
                if (!bar) {
                    continue;
                }
                uint16_t cmd = (uint16_t)pci_rd32((uint8_t)bus,
                                                  (uint8_t)dev,
                                                  (uint8_t)fn, 0x04);
                pci_wr16((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x04,
                         (uint16_t)(cmd | 0x6u));
                if (ctrl_start(&g_ctrl[ctrl_count],
                               (volatile uint32_t *)(uintptr_t)bar) != 0) {
                    continue;
                }
                ctrl_count++;
            }
        }
    }
    for (int ci = 0; ci < ctrl_count; ci++) {
        xhci_ctrl_t *c = &g_ctrl[ci];
        for (uint32_t p = 1; p <= c->max_ports; p++) {
            uint8_t speed = 0;
            if (port_reset(c, p, &speed) != 0) {
                continue;
            }
            uint32_t slot = 0;
            if (ctrl_command(c, XHCI_CMD_ENABLE_SLOT, 0, 0, &slot) != 0) {
                continue;
            }
            dev_enumerate(c, (uint8_t)slot, (uint8_t)p, speed);
        }
    }
}

int xhci_blk_count(void)
{
    if (!g_xhci_ready) {
        xhci_init();
    }
    int n = 0;
    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        if (g_dev[i].used && g_dev[i].storage) {
            n++;
        }
    }
    return n;
}

static xhci_dev_t *xhci_storage(int index)
{
    if (index < 0) {
        return NULL;
    }
    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        if (g_dev[i].used && g_dev[i].storage) {
            if (index-- == 0) {
                return &g_dev[i];
            }
        }
    }
    return NULL;
}

int xhci_blk_info(int index, fblk_info_t *out)
{
    if (!out) {
        return -1;
    }
    xhci_dev_t *d = xhci_storage(index);
    if (!d) {
        return -1;
    }
    finte_memset(out, 0, sizeof(*out));
    finte_strlcpy(out->name, d->name, sizeof(out->name));
    out->sectors = d->sectors;
    out->sector_size = 512;
    return 0;
}

int xhci_blk_read(int index, uint64_t lba, void *buf, uint32_t sectors)
{
    xhci_dev_t *d = xhci_storage(index);
    if (!d || !buf || sectors == 0 || lba >= d->sectors ||
        sectors > d->sectors - lba) {
        return -1;
    }
    hal_spin_lock(&g_xhci_io_lock);
    int rc = storage_read(d, lba, buf, sectors);
    hal_spin_unlock(&g_xhci_io_lock);
    return rc;
}

int xhci_blk_write(int index, uint64_t lba, const void *buf,
                   uint32_t sectors)
{
    xhci_dev_t *d = xhci_storage(index);
    if (!d || !buf || sectors == 0 || lba >= d->sectors ||
        sectors > d->sectors - lba) {
        return -1;
    }
    hal_spin_lock(&g_xhci_io_lock);
    int rc = storage_write(d, lba, buf, sectors);
    hal_spin_unlock(&g_xhci_io_lock);
    return rc;
}

void xhci_init(void)
{
    if (g_xhci_ready) {
        return;
    }
    g_xhci_ready = 1;
    xhci_scan();
}
