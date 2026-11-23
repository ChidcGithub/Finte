/* PS/2 scancode set 1 + COM1 serial input. Polled, no IRQs required. */
#include "input.h"
#include "xhci.h"
#include "finte/uefi.h"
#include "finte/hal.h"
#include "hal_x86.h"
#include <stdint.h>

#define COM1 0x3F8

static efi_simple_text_input_protocol_t *g_con_in;
static int g_input_usb;

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static int shift_down;
static int caps_lock;
static int e0_prefix;
static int serial_esc;
static int serial_num;
static int serial_mod;

typedef struct key_map {
    int code;
    int key;
} key_map_t;

static int key_lookup(const key_map_t *map, size_t count, int code)
{
    for (size_t i = 0; i < count; i++) {
        if (map[i].code == code) {
            return map[i].key;
        }
    }
    return -1;
}

static const key_map_t efi_scan_keys[] = {
    { 0x01, KEY_UP },
    { 0x02, KEY_DOWN },
    { 0x03, KEY_RIGHT },
    { 0x04, KEY_LEFT },
    { 0x05, KEY_HOME },
    { 0x06, KEY_END },
    { 0x08, KEY_DELETE },
    { 0x09, KEY_PGUP },
    { 0x0A, KEY_PGDN },
};

static const key_map_t serial_keys[] = {
    { 'A', KEY_UP },
    { 'B', KEY_DOWN },
    { 'C', KEY_RIGHT },
    { 'D', KEY_LEFT },
    { 'H', KEY_HOME },
    { 'F', KEY_END },
};

static const key_map_t serial_tilde_keys[] = {
    { 1, KEY_HOME },
    { 3, KEY_DELETE },
    { 4, KEY_END },
    { 5, KEY_PGUP },
    { 6, KEY_PGDN },
    { 7, KEY_HOME },
    { 8, KEY_END },
};

static const key_map_t ps2_e0_keys[] = {
    { 0x1C, '\n' },
    { 0x47, KEY_HOME },
    { 0x48, KEY_UP },
    { 0x49, KEY_PGUP },
    { 0x4B, KEY_LEFT },
    { 0x4D, KEY_RIGHT },
    { 0x4F, KEY_END },
    { 0x50, KEY_DOWN },
    { 0x51, KEY_PGDN },
    { 0x53, KEY_DELETE },
};

static int mouse_ok;
static int mouse_wheel_mode;
static int mouse_cycle;
static uint8_t mouse_pkt[4];
static int wheel_accum;

static const char keymap[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0F] = '\t', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u',
    [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[',
    [0x1B] = ']', [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z',
    [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' ',
};

static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0F] = '\t', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U',
    [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = '{',
    [0x1B] = '}', [0x1C] = '\n', [0x1E] = 'A', [0x1F] = 'S',
    [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|', [0x2C] = 'Z',
    [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>',
    [0x35] = '?', [0x39] = ' ',
};

static int ps2_wait_write(void)
{
    for (int i = 0; i < 20000; i++) {
        if ((inb(0x64) & 2) == 0) {
            return 0;
        }
    }
    return -1;
}

static int ps2_read_byte(uint8_t *out)
{
    for (int i = 0; i < 20000; i++) {
        if (inb(0x64) & 1) {
            *out = inb(0x60);
            return 0;
        }
    }
    return -1;
}

static int ps2_send_cmd(uint8_t cmd)
{
    if (ps2_wait_write() != 0) {
        return -1;
    }
    outb(0x64, cmd);
    return 0;
}

static int ps2_send_data(uint8_t data)
{
    if (ps2_wait_write() != 0) {
        return -1;
    }
    outb(0x60, data);
    return 0;
}

static int mouse_cmd(uint8_t cmd)
{
    uint8_t ack;
    if (ps2_send_cmd(0xD4) != 0 || ps2_send_data(cmd) != 0 ||
        ps2_read_byte(&ack) != 0) {
        return -1;
    }
    return ack == 0xFA ? 0 : -1;
}

static int mouse_arg(uint8_t arg)
{
    uint8_t ack;
    if (ps2_send_cmd(0xD4) != 0 || ps2_send_data(arg) != 0 ||
        ps2_read_byte(&ack) != 0) {
        return -1;
    }
    return ack == 0xFA ? 0 : -1;
}

static void mouse_byte(uint8_t b)
{
    if (mouse_cycle == 0 && !(b & 0x08)) {
        return; /* lost sync: wait for a packet start */
    }
    mouse_pkt[mouse_cycle++] = b;
    int need = mouse_wheel_mode ? 4 : 3;
    if (mouse_cycle < need) {
        return;
    }
    mouse_cycle = 0;
    if (mouse_wheel_mode) {
        int wheel = mouse_pkt[3] & 0x0F;
        if (wheel & 0x08) {
            wheel -= 16;
        }
        wheel_accum += wheel;
    }
}

static void mouse_init(void)
{
    uint8_t cfg;
    mouse_ok = 0;
    mouse_wheel_mode = 0;
    mouse_cycle = 0;
    wheel_accum = 0;

    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) {
        (void)inb(0x60);
    }
    if (ps2_send_cmd(0xA8) != 0) {
        return; /* enable auxiliary port */
    }
    if (ps2_send_cmd(0x20) != 0 || ps2_read_byte(&cfg) != 0) {
        return; /* read controller configuration */
    }
    cfg = (uint8_t)((cfg & ~0x20) | 0x02);
    if (ps2_send_cmd(0x60) != 0 || ps2_send_data(cfg) != 0) {
        return;
    }
    if (mouse_cmd(0xF6) != 0) {
        return; /* reset to defaults */
    }
    if (mouse_cmd(0xF3) != 0 || mouse_arg(200) != 0) {
        return;
    }
    if (mouse_cmd(0xF3) != 0 || mouse_arg(100) != 0) {
        return;
    }
    if (mouse_cmd(0xF3) != 0 || mouse_arg(80) != 0) {
        return; /* sample-rate magic enables IntelliMouse packets */
    }
    uint8_t id = 0;
    if (mouse_cmd(0xF2) != 0 || ps2_read_byte(&id) != 0) {
        return; /* get device id */
    }
    mouse_wheel_mode = (id == 3 || id == 4);
    if (mouse_cmd(0xF4) != 0) {
        return; /* enable data reporting */
    }
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) {
        (void)inb(0x60);
    }
    mouse_ok = 1;
}

void input_init(void)
{
    /* Drain any stale controller bytes. The BIOS leaves the keyboard enabled. */
    for (int i = 0; i < 32 && (inb(0x64) & 1); i++) {
        (void)inb(0x60);
    }
    shift_down = 0;
    caps_lock = 0;
    e0_prefix = 0;
    serial_esc = 0;
    serial_num = 0;
    serial_mod = 0;
    mouse_init();
    xhci_init();
}

void input_boot(const fboot_info_t *bi)
{
    g_con_in = NULL;
    g_input_usb = 0;
    if (bi && bi->magic == FINTE_BOOT_MAGIC &&
        bi->console_type == FINTE_BOOT_CONSOLE_UEFI && bi->fw_services) {
        g_con_in = (efi_simple_text_input_protocol_t *)(uintptr_t)bi->efi_con_in;
        g_input_usb = (int)bi->input_usb;
    }
}

static int uefi_trykey(void)
{
    if (!g_con_in || !g_con_in->ReadKeyStroke) {
        return -1;
    }
    EFI_INPUT_KEY key;
    if (!hal_fw_begin()) {
        return -1;
    }
    EFI_STATUS st = g_con_in->ReadKeyStroke(g_con_in, &key);
    hal_fw_end();
    if (st != 0) {
        return -1;
    }
    int mapped = key_lookup(efi_scan_keys,
                            FINTE_ARRAY_SIZE(efi_scan_keys),
                            key.ScanCode);
    if (mapped >= 0) {
        return mapped;
    }
    if (key.UnicodeChar == 0) {
        return -1;
    }
    if (key.UnicodeChar == '\n' || key.UnicodeChar == '\r') {
        return '\n';
    }
    if (key.UnicodeChar == '\b' || key.UnicodeChar == 0x7F) {
        return '\b';
    }
    if (key.UnicodeChar == '\t') {
        return '\t';
    }
    if (key.UnicodeChar >= 32 && key.UnicodeChar < 127) {
        return (int)key.UnicodeChar;
    }
    return -1;
}

static int serial_trykey(void)
{
    unsigned char lsr = inb(COM1 + 5);
    if (lsr == 0xFF || (lsr & 0x01) == 0) {
        return -1;
    }
    unsigned char c = inb(COM1);
    if (lsr == 0xFE || c == 0xFF) {
        return -1;
    }
    if (serial_esc == 0) {
        if (c == 0x1B) {
            serial_esc = 1;
            return -1;
        }
        if (c == '\r') {
            return '\n';
        }
        if (c == 0x7F) {
            return '\b';
        }
        return c;
    }
    if (serial_esc == 1) {
        if (c == '[') {
            serial_esc = 2;
            return -1;
        }
        serial_esc = 0;
        return -1;
    }
    if (serial_esc == 2) {
        if (c >= '0' && c <= '9') {
            serial_num = c - '0';
            serial_esc = 3;
            return -1;
        }
        serial_esc = 0;
        return key_lookup(serial_keys, FINTE_ARRAY_SIZE(serial_keys), c);
    }
    if (serial_esc == 3) {
        if (c == ';') {
            serial_mod = 0;
            serial_esc = 4;
            return -1;
        }
        serial_esc = 0;
        if (c == '~') {
            return key_lookup(serial_tilde_keys,
                              FINTE_ARRAY_SIZE(serial_tilde_keys),
                              serial_num);
        }
        return -1;
    }
    if (serial_esc == 4) {
        if (c >= '0' && c <= '9') {
            serial_mod = serial_mod * 10 + (c - '0');
            return -1;
        }
        serial_esc = 0;
        int shifted = (serial_mod & 2) != 0;
        int mapped = key_lookup(serial_keys,
                                FINTE_ARRAY_SIZE(serial_keys), c);
        if (shifted && mapped == KEY_UP) {
            return KEY_SHIFT_UP;
        }
        if (shifted && mapped == KEY_DOWN) {
            return KEY_SHIFT_DOWN;
        }
        return mapped;
    }
    return -1;
}

static int ps2_trykey(void)
{
    uint8_t status = inb(0x64);
    if ((status & 1) == 0) {
        return -1;
    }
    unsigned char sc = inb(0x60);
    if (status & 0x20) {
        if (mouse_ok) {
            mouse_byte((uint8_t)sc);
        }
        return -1;
    }
    if (sc == 0xE0) {
        e0_prefix = 1;
        return -1;
    }
    if (e0_prefix) {
        e0_prefix = 0;
        if (sc & 0x80) {
            return -1;
        }
        int mapped = key_lookup(ps2_e0_keys,
                                FINTE_ARRAY_SIZE(ps2_e0_keys), sc);
        if (mapped == KEY_UP && shift_down) {
            return KEY_SHIFT_UP;
        }
        if (mapped == KEY_DOWN && shift_down) {
            return KEY_SHIFT_DOWN;
        }
        return mapped;
    }
    if (sc == 0x2A || sc == 0x36) {
        shift_down = 1;
        return -1;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift_down = 0;
        return -1;
    }
    if (sc == 0x3A) {
        caps_lock = !caps_lock;
        return -1;
    }
    if (sc & 0x80) {
        return -1;
    }
    if (sc == 0x0E) {
        return '\b';
    }
    char ch = shift_down ? keymap_shift[sc] : keymap[sc];
    if (ch >= 'a' && ch <= 'z' && caps_lock) {
        ch = (char)(ch - 'a' + 'A');
    } else if (ch >= 'A' && ch <= 'Z' && caps_lock) {
        ch = (char)(ch - 'A' + 'a');
    }
    return ch ? (unsigned char)ch : -1;
}

int input_trywheel(void)
{
    for (int i = 0; i < 64; i++) {
        uint8_t status = inb(0x64);
        if ((status & 1) == 0) {
            break;
        }
        if ((status & 0x20) == 0) {
            break; /* a keyboard byte is waiting; leave it for input_trykey */
        }
        uint8_t data = inb(0x60);
        if (mouse_ok) {
            mouse_byte(data);
        }
    }
    /* The PS/2 packet is negative for wheel-up (Linux negates it too). */
    int wheel = -wheel_accum;
    wheel_accum = 0;
    return wheel;
}

int input_trykey(void)
{
    int c = serial_trykey();
    if (c >= 0) {
        return c;
    }
    c = xhci_trykey();
    if (c >= 0) {
        return c;
    }
    if (g_con_in && g_input_usb) {
        return uefi_trykey();
    }
    return ps2_trykey();
}
