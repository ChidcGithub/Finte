/* Signal-timer test: alarm cancel/remaining, one-shot SIGALRM, setitimer
 * intervals, getitimer and the default SIGALRM terminate action. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_FORK 9
#define SYS_SIGNAL 12
#define SYS_SIGRETURN 13
#define SYS_GETITIMER 36
#define SYS_ALARM 37
#define SYS_SETITIMER 38
#define SYS_GETPID 39
#define SYS_WAITPID 40
#define SYS_EXIT 60
#define SYS_TIME 200
#define SYS_SLEEP 201
#define SYS_YIELD 202

#define SIGALRM 14

struct timeval {
    long sec;
    long usec;
};

struct itimerval {
    struct timeval interval;
    struct timeval value;
};

static volatile unsigned long alarm_count;

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

static void on_alarm(int sig)
{
    (void)sig;
    alarm_count++;
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

static int wait_alarms(unsigned long target, unsigned long timeout_ns)
{
    long start = sc(SYS_TIME, 0, 0, 0);
    while (alarm_count < target) {
        if ((unsigned long)(sc(SYS_TIME, 0, 0, 0) - start) > timeout_ns) {
            return 0;
        }
        sc(SYS_SLEEP, 100000000ull, 0, 0);
    }
    return 1;
}

void _start(void)
{
    int ok = 1;
    ok = ok && install(SIGALRM, on_alarm);

    /* alarm() reports the remaining time and cancels with zero. */
    ok = ok && sc(SYS_ALARM, 0, 0, 0) == 0;
    ok = ok && sc(SYS_ALARM, 1, 0, 0) == 0;
    long rem = sc(SYS_ALARM, 2, 0, 0);
    ok = ok && (rem == 1 || rem == 2);
    rem = sc(SYS_ALARM, 0, 0, 0);
    ok = ok && (rem == 1 || rem == 2);
    ok = ok && sc(SYS_SLEEP, 1200000000ull, 0, 0) == 0;
    ok = ok && alarm_count == 0;

    /* A one-shot alarm fires once and disarms itself. */
    ok = ok && sc(SYS_ALARM, 1, 0, 0) == 0;
    ok = ok && wait_alarms(1, 3000000000ull);
    ok = ok && sc(SYS_ALARM, 0, 0, 0) == 0;

    /* ITIMER_REAL repeats and getitimer reports the armed values. */
    struct itimerval it;
    struct itimerval old;
    zero_timer(&it);
    zero_timer(&old);
    it.value.usec = 200000;
    it.interval.usec = 200000;
    alarm_count = 0;
    ok = ok && sc(SYS_SETITIMER, 0, (long)&it, (long)&old) == 0;
    ok = ok && old.value.sec == 0 && old.value.usec == 0;
    ok = ok && old.interval.sec == 0 && old.interval.usec == 0;

    struct itimerval got;
    zero_timer(&got);
    ok = ok && sc(SYS_GETITIMER, 0, (long)&got, 0) == 0;
    ok = ok && got.interval.sec == 0 && got.interval.usec == 200000;
    ok = ok && got.value.usec <= 200000;
    ok = ok && wait_alarms(2, 2500000000ull);

    struct itimerval zero;
    zero_timer(&zero);
    ok = ok && sc(SYS_SETITIMER, 0, (long)&zero, (long)&old) == 0;
    ok = ok && old.interval.usec == 200000;
    unsigned long frozen = alarm_count;
    ok = ok && sc(SYS_SLEEP, 600000000ull, 0, 0) == 0;
    ok = ok && alarm_count == frozen;

    /* The default SIGALRM action terminates the child. */
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_SIGNAL, SIGALRM, 0, 0);
        sc(SYS_ALARM, 1, 0, 0);
        for (;;) {
            sc(SYS_GETPID, 0, 0, 0);
            sc(SYS_YIELD, 0, 0, 0);
        }
    }
    int status = -1;
    long w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && pid > 0 && w == pid && ((status & 0x7F) == 0) &&
         (((status >> 8) & 0xFF) == 128 + SIGALRM);

    print(ok ? "[alarm] OK\n" : "[alarm] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
