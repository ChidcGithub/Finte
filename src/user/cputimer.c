/* CPU-time timers: ITIMER_VIRTUAL and ITIMER_PROF only advance while the
 * process runs, and the matching signals invoke their handlers. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_SIGNAL 12
#define SYS_SIGRETURN 13
#define SYS_GETITIMER 36
#define SYS_SETITIMER 38
#define SYS_EXIT 60
#define SYS_TIME 200
#define SYS_SLEEP 201

#define SIGVTALRM 26
#define SIGPROF 27

#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

struct timeval {
    long sec;
    long usec;
};

struct itimerval {
    struct timeval interval;
    struct timeval value;
};

static volatile unsigned long vt_count;
static volatile unsigned long prof_count;

static long sc(long nr, long a, long b, long c)
{
    return fuser_syscall(nr, a, b, c);
}

static void print(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    sc(SYS_WRITE, (long)s, n, 0);
}

static void on_vt(int sig)
{
    (void)sig;
    vt_count++;
}

static void on_prof(int sig)
{
    (void)sig;
    prof_count++;
}

static void restorer(void)
{
    sc(SYS_SIGRETURN, 0, 0, 0);
}

static int install(int sig, void (*handler)(int))
{
    return sc(SYS_SIGNAL, sig, (long)handler, (long)restorer) == 0;
}

static void zero_timer(struct itimerval *it)
{
    it->interval.sec = 0;
    it->interval.usec = 0;
    it->value.sec = 0;
    it->value.usec = 0;
}

static int wait_counter(volatile unsigned long *counter,
                        unsigned long target, unsigned long timeout_ns)
{
    long start = sc(SYS_TIME, 0, 0, 0);
    while (*counter < target) {
        if ((unsigned long)(sc(SYS_TIME, 0, 0, 0) - start) > timeout_ns) {
            return 0;
        }
    }
    return 1;
}

void _start(void)
{
    int ok = 1;
    ok = ok && install(SIGVTALRM, on_vt);
    ok = ok && install(SIGPROF, on_prof);

    struct itimerval it;
    struct itimerval got;
    struct itimerval zero;
    zero_timer(&it);
    zero_timer(&got);
    zero_timer(&zero);

    /* Virtual time is CPU time: a 300 ms sleep must not fire it. */
    it.value.usec = 100000;
    it.interval.usec = 100000;
    ok = ok && sc(SYS_SETITIMER, ITIMER_VIRTUAL, (long)&it, 0) == 0;
    ok = ok && sc(SYS_GETITIMER, ITIMER_VIRTUAL, (long)&got, 0) == 0;
    ok = ok && got.interval.usec == 100000 && got.value.usec <= 100000;
    ok = ok && sc(SYS_SLEEP, 300000000ull, 0, 0) == 0;
    ok = ok && vt_count == 0;

    /* A pure user loop must be interrupted by the CPU timer IRQ, without
     * relying on a syscall to poll pending signals. */
    volatile unsigned long spin = 0;
    while (vt_count < 2 && spin < 500000000ul) {
        spin++;
    }
    ok = ok && vt_count >= 2;
    ok = ok && sc(SYS_SETITIMER, ITIMER_VIRTUAL, (long)&zero, 0) == 0;
    unsigned long vt_frozen = vt_count;
    ok = ok && sc(SYS_SLEEP, 300000000ull, 0, 0) == 0;
    ok = ok && vt_count == vt_frozen;

    /* Prof time covers user and kernel execution and repeats. */
    zero_timer(&it);
    it.value.usec = 100000;
    it.interval.usec = 100000;
    ok = ok && sc(SYS_SETITIMER, ITIMER_PROF, (long)&it, 0) == 0;
    ok = ok && wait_counter(&prof_count, 2, 3000000000ull);
    ok = ok && sc(SYS_SETITIMER, ITIMER_PROF, (long)&zero, 0) == 0;
    unsigned long prof_frozen = prof_count;
    ok = ok && sc(SYS_SLEEP, 300000000ull, 0, 0) == 0;
    ok = ok && prof_count == prof_frozen;

    ok = ok && sc(SYS_SETITIMER, 3, (long)&it, 0) < 0;
    ok = ok && sc(SYS_GETITIMER, 3, (long)&got, 0) < 0;

    print(ok ? "[cputime] OK\n" : "[cputime] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
