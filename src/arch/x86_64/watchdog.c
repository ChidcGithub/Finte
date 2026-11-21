/* Three-strike system watchdog.
 *
 * Unexpected user faults, scheduler deadlocks and disk-sync failures each add
 * a strike.  Three strikes print a panic and power off.  The self-check suite
 * is suspended because it intentionally provokes a deadlock. */
#include "finte/finte.h"

#define WATCHDOG_LIMIT 3

#define WD_RED    "\x1b[1;31m"
#define WD_YELLOW "\x1b[33m"
#define WD_CYAN   "\x1b[1;36m"
#define WD_RESET  "\x1b[0m"

static int g_strikes;
static int g_suspend;
static char g_last[96];

static void watchdog_panic(const char *reason) __attribute__((noreturn));

static void watchdog_panic(const char *reason)
{
    hal_timer_stop_cpu();
    hal_irq_disable();
    char buf[224];
    int n = finte_format(buf, sizeof(buf),
                         "\n" WD_RED "[watchdog] PANIC after %d abnormal "
                         "events: %s" WD_RESET "\n",
                         WATCHDOG_LIMIT, reason ? reason : "unknown");
    hal_console_write(buf, (size_t)n);
    hal_console_write(WD_CYAN "[watchdog]" WD_RESET " shutdown\n", 31);
    hal_idle(250000000ull);
    hal_exit(0x70);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void hal_watchdog_strike(const char *reason)
{
    if (g_suspend > 0 || g_strikes >= WATCHDOG_LIMIT) {
        return;
    }
    g_strikes++;
    finte_strlcpy(g_last, reason ? reason : "unknown", sizeof(g_last));
    char buf[192];
    int n = finte_format(buf, sizeof(buf),
                         WD_YELLOW "[watchdog] abnormal %d/%d: %s" WD_RESET
                         "\n",
                         g_strikes, WATCHDOG_LIMIT, g_last);
    hal_console_write(buf, (size_t)n);
    if (g_strikes >= WATCHDOG_LIMIT) {
        watchdog_panic(g_last);
    }
}

void hal_watchdog_suspend(void)
{
    g_suspend++;
}

void hal_watchdog_resume(void)
{
    if (g_suspend > 0) {
        g_suspend--;
    }
}

int hal_watchdog_strikes(void)
{
    return g_strikes;
}

const char *hal_watchdog_last(void)
{
    return g_last;
}

void hal_watchdog_clear(void)
{
    g_strikes = 0;
    g_last[0] = 0;
}
