/* Service client: exercise register/lookup/unregister on its own channel,
 * then resolve the echo service and its reply channel through the registry. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_EXIT 60
#define SYS_SERVICE_REG 15
#define SYS_SERVICE_LOOK 16
#define SYS_SERVICE_UNREG 17
#define SYS_IPC_SEND 210
#define SYS_IPC_RECV 211

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

void _start(void)
{
    static const char self_name[] = "svc-self";
    static const char req_name[] = "echo";
    static const char rep_name[] = "echo-reply";
    char buf[8];
    int ok = 1;

    long reg = sc(SYS_SERVICE_REG, (long)self_name,
                  (long)(sizeof(self_name) - 1), 0);
    ok = ok && reg == 0;
    long self_cap = sc(SYS_SERVICE_LOOK, (long)self_name,
                       (long)(sizeof(self_name) - 1), 0);
    ok = ok && self_cap >= 0;
    long unreg = sc(SYS_SERVICE_UNREG, (long)self_name,
                    (long)(sizeof(self_name) - 1), 0);
    ok = ok && unreg == 0;
    long gone = sc(SYS_SERVICE_LOOK, (long)self_name,
                   (long)(sizeof(self_name) - 1), 0);
    ok = ok && gone < 0;

    long req = sc(SYS_SERVICE_LOOK, (long)req_name,
                  (long)(sizeof(req_name) - 1), 0);
    long rep = sc(SYS_SERVICE_LOOK, (long)rep_name,
                  (long)(sizeof(rep_name) - 1), 0);
    ok = ok && req >= 0 && rep >= 0;
    if (ok) {
        long sent = sc(SYS_IPC_SEND, req, (long)"ping", 4);
        long n = sc(SYS_IPC_RECV, rep, (long)buf, 4);
        ok = sent == 4 && n == 4 && buf[0] == 'p' && buf[1] == 'o';
    }
    print(ok ? "[svc] client OK\n" : "[svc] client FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
