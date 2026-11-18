/*
 * widget_boot: optional, temporary process priority experiment for the S9.
 *
 * Weather/Calendar WidgetKit extensions can each occupy a full CPU core while
 * SpringBoard draws the first home screen. A positive nice value after the
 * extensions have survived launchd's 5-second check-in may improve UI latency.
 * This does not stop widgets or guarantee lower total CPU power.
 *
 * widget_nice=N (0 default: off; 1..10: temporary nice value). Restore by 150s.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PROC_ALL_PIDS 1
#define MAX_PROCS 2048
#define MAX_TRACKED 48
#define SCAN_SECONDS 5
#define CHECKIN_GRACE 15
#define BOOT_WINDOW 150

int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

struct widget {
    int pid;
    int seen_at;
    int original;
    int adjusted;
    char name[80];
};

static int pids[MAX_PROCS];
static struct widget tracked[MAX_TRACKED];
static int tracked_count;
static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static int target(const char *name)
{
    return strcmp(name, "CalendarWidgetExtension") == 0 ||
           strcmp(name, "WeatherWidget") == 0;
}

static int boot_nice(void)
{
    char args[1024] = "";
    size_t len = sizeof(args) - 1;
    const char *needle = "widget_nice=", *at;

    if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0)
        return 0;
    at = args;
    while ((at = strstr(at, needle)) != NULL) {
        if (at == args || at[-1] == ' ') {
            char *end;
            long value = strtol(at + strlen(needle), &end, 10);

            if ((*end == '\0' || *end == ' ') && value >= 1 && value <= 10)
                return (int)value;
            return 0;
        }
        at++;
    }
    return 0;
}

static void scan(int seconds, int requested)
{
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
    char name[256];

    if (bytes <= 0)
        return;
    for (int i = 0; i < bytes / (int)sizeof(int); i++) {
        int pid = pids[i], index;

        if (pid <= 0 || proc_name(pid, name, sizeof(name)) <= 0 || !target(name))
            continue;
        for (index = 0; index < tracked_count; index++) {
            if (tracked[index].pid == pid && strcmp(tracked[index].name, name) == 0)
                break;
        }
        if (index == tracked_count) {
            if (tracked_count >= MAX_TRACKED)
                continue;
            struct widget *w = &tracked[tracked_count++];

            w->pid = pid;
            w->seen_at = seconds;
            w->original = 0;
            w->adjusted = 0;
            snprintf(w->name, sizeof(w->name), "%s", name);
        }
        struct widget *w = &tracked[index];

        if (w->adjusted || seconds - w->seen_at < CHECKIN_GRACE)
            continue;
        errno = 0;
        int old = getpriority(PRIO_PROCESS, pid);

        if (old == -1 && errno != 0) {
            printf("widget_boot: cannot read priority of %s[%d] (errno=%d)\n",
                   name, pid, errno);
            w->adjusted = -1;
            continue;
        }
        if (old >= requested) {
            w->adjusted = -1;
            continue;
        }
        if (setpriority(PRIO_PROCESS, pid, requested) != 0) {
            printf("widget_boot: cannot lower priority of %s[%d] (errno=%d)\n",
                   name, pid, errno);
            w->adjusted = -1;
            continue;
        }
        w->original = old;
        w->adjusted = 1;
        printf("widget_boot: %s[%d] nice %d -> %d at %d s (check-in grace %d s)\n",
               name, pid, old, requested, seconds, CHECKIN_GRACE);
    }
}

static void restore(int requested)
{
    char name[256];

    for (int i = 0; i < tracked_count; i++) {
        struct widget *w = &tracked[i];

        if (w->adjusted != 1 || proc_name(w->pid, name, sizeof(name)) <= 0 ||
            strcmp(name, w->name) != 0)
            continue; /* exited process, or PID recycled */
        errno = 0;
        int current = getpriority(PRIO_PROCESS, w->pid);

        if ((current == -1 && errno != 0) || current != requested)
            continue; /* another service changed its priority */
        if (setpriority(PRIO_PROCESS, w->pid, w->original) == 0)
            printf("widget_boot: %s[%d] nice %d -> %d restored\n",
                   name, w->pid, requested, w->original);
        else
            printf("widget_boot: cannot restore priority of %s[%d] (errno=%d)\n",
                   name, w->pid, errno);
    }
}

int main(void)
{
    int requested = boot_nice();

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (requested == 0) {
        printf("widget_boot: off (widget_nice=0/default)\n");
        return 0;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    printf("widget_boot: nice=%d for CalendarWidgetExtension and WeatherWidget, "
           "check-in grace=%d s, restore by %d s\n",
           requested, CHECKIN_GRACE, BOOT_WINDOW);
    for (int seconds = 0; seconds < BOOT_WINDOW && !interrupted;
         seconds += SCAN_SECONDS) {
        scan(seconds, requested);
        sleep(SCAN_SECONDS);
    }
    restore(requested);
    printf("widget_boot: finished; %d target processes observed\n", tracked_count);
    return 0;
}
