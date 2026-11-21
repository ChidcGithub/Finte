/* Isolated process demo: IPC, shared memory, time, isolation and preemption.
 *
 * PID 1 sends a message and then probes supervisor memory (the kernel kills it).
 * PID 2 receives, checks that the same virtual address holds a private value,
 * checks shared memory and sleeps. PID 3 busy-spins while PID 4 wakes from
 * sleeps and updates a shared heartbeat, proving timer preemption. */
#include <stdint.h>

#define SYS_WRITE 1
#define SYS_GETPID 39
#define SYS_EXIT 60
#define SYS_TIME 200
#define SYS_SLEEP 201
#define SYS_IPC_SEND 210
#define SYS_IPC_RECV 211
#define SYS_SHM_ADDR 220

#define SHM_MAGIC 0
#define SHM_HEARTBEAT 8
#define SHM_FLAGS 16
#define SHM_SECRET_ADDR 24
#define SHM_SECRET_VALUE 32
#define SHM_ELAPSED 40
#define SHM_SPIN 48

#define MAGIC 0xF17E5EED12345678ull
#define IPC_TAG 0x5EED5EEDull

#define FLAG_IPC (1ull << 0)
#define FLAG_ISO (1ull << 1)
#define FLAG_SHM (1ull << 2)
#define FLAG_TIME (1ull << 3)
#define FLAG_PREEMPT (1ull << 4)

static volatile uint64_t secret;
static volatile uint64_t sink;

static long sys1(long nr, long a)
{
    register long rax __asm__("rax") = nr;
    register long rdi __asm__("rdi") = a;
    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi)
                     : "memory");
    return rax;
}

static long sys3(long nr, long a, long b, long c)
{
    register long rax __asm__("rax") = nr;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "memory");
    return rax;
}

static void print(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    sys3(SYS_WRITE, (long)s, n, 0);
}

static void print_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char out[18];
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    sys3(SYS_WRITE, (long)out, (long)sizeof(out), 0);
}

static void print_flag(int ok)
{
    print(ok ? "OK" : "FAIL");
}

static void exit_now(long code)
{
    sys1(SYS_EXIT, code);
    for (;;) {
    }
}

static void role_sender(volatile uint64_t *shm)
{
    secret = 0x1111111122222222ull;
    shm[SHM_MAGIC / 8] = MAGIC;
    shm[SHM_SECRET_ADDR / 8] = (uint64_t)&secret;
    shm[SHM_SECRET_VALUE / 8] = secret;

    uint64_t msg[3];
    msg[0] = (uint64_t)&secret;
    msg[1] = secret;
    msg[2] = IPC_TAG;
    long n = sys3(SYS_IPC_SEND, 0, (long)msg, (long)sizeof(msg));
    print("[pid 1] ipc send=");
    print_hex((uint64_t)n);
    print(" secret=");
    print_hex(secret);
    print("\n[pid 1] probing kernel memory at 0x4000000\n");
    volatile uint64_t *kernel = (volatile uint64_t *)0x4000000ull;
    (void)*kernel;
    print("[pid 1] ERROR: kernel memory was user-readable\n");
    exit_now(1);
}

static void role_receiver(volatile uint64_t *shm)
{
    secret = 0x3333333344444444ull;
    uint64_t msg[3] = { 0, 0, 0 };
    long n = sys3(SYS_IPC_RECV, 0, (long)msg, (long)sizeof(msg));

    uint64_t t0 = (uint64_t)sys1(SYS_TIME, 0);
    sys1(SYS_SLEEP, 50000000);
    uint64_t t1 = (uint64_t)sys1(SYS_TIME, 0);

    int ipc_ok = n == (long)sizeof(msg) && msg[2] == IPC_TAG;
    int iso_ok = ipc_ok && msg[0] == (uint64_t)&secret &&
                 msg[1] != secret &&
                 *(volatile uint64_t *)(uintptr_t)msg[0] == secret;
    int shm_ok = shm[SHM_MAGIC / 8] == MAGIC;
    int time_ok = t1 >= t0 && t1 - t0 >= 45000000ull;

    if (ipc_ok) {
        shm[SHM_FLAGS / 8] |= FLAG_IPC;
    }
    if (iso_ok) {
        shm[SHM_FLAGS / 8] |= FLAG_ISO;
    }
    if (shm_ok) {
        shm[SHM_FLAGS / 8] |= FLAG_SHM;
    }
    if (time_ok) {
        shm[SHM_FLAGS / 8] |= FLAG_TIME;
    }
    shm[SHM_ELAPSED / 8] = t1 - t0;

    print("[pid 2] ipc=");
    print_flag(ipc_ok);
    print(" isolation=");
    print_flag(iso_ok);
    print(" shm=");
    print_flag(shm_ok);
    print(" sleep=");
    print_flag(time_ok);
    print(" elapsed=");
    print_hex(t1 - t0);
    print("ns\n");
    exit_now(0);
}

static void role_spinner(volatile uint64_t *shm)
{
    uint64_t t0 = (uint64_t)sys1(SYS_TIME, 0);
    uint64_t count = 0;
    for (;;) {
        for (uint64_t i = 0; i < 200000; i++) {
            count += i;
        }
        sink = count;
        uint64_t now = (uint64_t)sys1(SYS_TIME, 0);
        if (now - t0 >= 200000000ull) {
            break;
        }
    }
    uint64_t hb = shm[SHM_HEARTBEAT / 8];
    shm[SHM_SPIN / 8] = count;
    if (hb > 0) {
        shm[SHM_FLAGS / 8] |= FLAG_PREEMPT;
    }
    print("[pid 3] spin=");
    print_hex(count);
    print(" heartbeat=");
    print_hex(hb);
    print("\n");
    exit_now(0);
}

static void role_watchdog(volatile uint64_t *shm)
{
    for (int i = 0; i < 3; i++) {
        sys1(SYS_SLEEP, 20000000);
        shm[SHM_HEARTBEAT / 8]++;
    }
    print("[pid 4] heartbeat=");
    print_hex(shm[SHM_HEARTBEAT / 8]);
    print("\n");
    exit_now(0);
}

void _start(void)
{
    long pid = sys1(SYS_GETPID, 0);
    uint64_t addr = (uint64_t)sys1(SYS_SHM_ADDR, 0);
    volatile uint64_t *shm = (volatile uint64_t *)(uintptr_t)addr;
    if (!shm) {
        print("[user] no shared mapping\n");
        exit_now(1);
    }
    if (pid == 1) {
        role_sender(shm);
    } else if (pid == 2) {
        role_receiver(shm);
    } else if (pid == 3) {
        role_spinner(shm);
    } else if (pid == 4) {
        role_watchdog(shm);
    } else {
        print("[user] unknown pid\n");
    }
    exit_now(0);
}
