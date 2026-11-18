/* procs.c - reads the running programs from /proc (see procs.h). */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "procs.h"

static char root[200] = "/proc";

void procs_set_root(const char *r) { snprintf(root, sizeof root, "%s", r && r[0] ? r : "/proc"); }
const char *procs_root(void) { return root; }

/* "123" -> 123; anything that isn't only digits -> -1 */
static int folder_pid(const char *s)
{
    int v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 9999999) return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

int procs_read(int pid, ProcInfo *p)
{
    char path[260], line[160];
    memset(p, 0, sizeof *p);
    snprintf(path, sizeof path, "%s/%d/status", root, pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int has_memory = 0;                     /* real programs have "Vm..." lines, kernel threads don't */
    p->pid = pid;
    p->state = '?';
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, ':');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, "\n")] = 0;
        if (!strcmp(line, "Name")) snprintf(p->name, sizeof p->name, "%s", v);
        else if (!strcmp(line, "State")) p->state = v[0] ? v[0] : '?';
        else if (!strcmp(line, "PPid")) p->ppid = atoi(v);
        else if (!strcmp(line, "VmRSS")) { p->rss_kb = atol(v); has_memory = 1; }
        else if (!strncmp(line, "Vm", 2)) has_memory = 1;
    }
    fclose(f);
    p->kernel = pid == 2 || p->ppid == 2 || (!has_memory && p->state != 'Z' && pid != 1);
    return 0;
}

/* biggest memory first; same memory: smaller PID first (so the order doesn't jump) */
static int by_memory(const void *a, const void *b)
{
    const ProcInfo *x = a, *y = b;
    if (x->rss_kb != y->rss_kb) return x->rss_kb < y->rss_kb ? 1 : -1;
    return x->pid - y->pid;
}

int procs_list(ProcInfo *out, int max)
{
    int n = 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        int pid = folder_pid(e->d_name);
        if (pid > 0 && procs_read(pid, &out[n]) == 0) n++;
    }
    closedir(d);
    qsort(out, n, sizeof out[0], by_memory);
    return n;
}

/* the first number in root/<file> (e.g. "loadavg": "0.45 0.30 ...") */
static double first_number(const char *file)
{
    char path[260];
    double v = 0;
    snprintf(path, sizeof path, "%s/%s", root, file);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%lf", &v) != 1) v = 0;
    fclose(f);
    return v;
}

int procs_summary(ProcSummary *s)
{
    char path[260], line[128];
    memset(s, 0, sizeof *s);
    s->mem_avail_kb = -1;
    long mem_free = 0;
    snprintf(path, sizeof path, "%s/meminfo", root);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        long v = 0;
        char key[40];
        if (sscanf(line, "%39[^:]: %ld", key, &v) != 2) continue;
        if (!strcmp(key, "MemTotal")) s->mem_total_kb = v;
        else if (!strcmp(key, "MemAvailable")) s->mem_avail_kb = v;
        else if (!strcmp(key, "MemFree")) mem_free = v;
        else if (!strcmp(key, "SwapTotal")) s->swap_total_kb = v;
        else if (!strcmp(key, "SwapFree")) s->swap_free_kb = v;
    }
    fclose(f);
    if (s->mem_avail_kb < 0) s->mem_avail_kb = mem_free;    /* very old kernels: no MemAvailable */
    s->load1 = first_number("loadavg");
    s->uptime_s = first_number("uptime");
    return 0;
}

int procs_cpu_times(unsigned long long *busy, unsigned long long *total)
{
    char path[260];
    unsigned long long v[8] = { 0 };       /* user nice system idle iowait irq softirq steal */
    snprintf(path, sizeof path, "%s/stat", root);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return -1;
    *total = 0;
    for (int i = 0; i < 8; i++) *total += v[i];
    *busy = *total - v[3] - v[4];           /* everything except idle and waiting for the SD card */
    return 0;
}

int procs_alive(int pid)
{
    ProcInfo p;
    return procs_read(pid, &p) == 0 && p.state != 'Z' && p.state != 'X';
}

const char *procs_state_word(char state)
{
    switch (state) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "waiting";
    case 'T': case 't': return "paused";
    case 'Z': return "finished";
    case 'I': return "idle";
    default:  return "unknown";
    }
}

/* the program's whole command line, with spaces between the words */
static void read_cmdline(int pid, char *buf, size_t n)
{
    char path[260];
    buf[0] = 0;
    snprintf(path, sizeof path, "%s/%d/cmdline", root, pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t len = fread(buf, 1, n - 1, f);
    fclose(f);
    for (size_t i = 0; i < len; i++) if (!buf[i]) buf[i] = ' ';   /* the words are split by 0 bytes */
    buf[len] = 0;
}

/* is `pid` NZEUS itself, or one of the programs that started it? */
static int in_my_family(int pid, int self_pid)
{
    int cur = self_pid;
    for (int steps = 0; cur > 1 && steps < 32; steps++) {   /* walk up: me, my parent, its parent... */
        if (cur == pid) return 1;
        ProcInfo p;
        if (procs_read(cur, &p) != 0) break;
        cur = p.ppid;
    }
    return 0;
}

const char *procs_protected(const ProcInfo *p, int self_pid)
{
    static const struct { const char *name, *why; } keep[] = {
        { "nzeus-deck",      "it starts and stops NZEUS Deck" },
        { "dropbear",        "the SSH server: the PC's way in" },
        { "nzeus-usb-shell", "the USB serial rescue shell" },
    };
    char cmd[256];
    if (p->pid <= 1) return "Linux needs it (it is PID 1, the first program)";
    if (p->kernel) return "it is part of Linux itself (a kernel thread)";
    if (p->state == 'Z') return "it has already finished";
    if (p->pid == self_pid) return "that is NZEUS Deck itself";
    if (in_my_family(p->pid, self_pid)) return "NZEUS Deck needs it (it started NZEUS Deck)";
    read_cmdline(p->pid, cmd, sizeof cmd);
    for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) {
        size_t n = strlen(keep[i].name);
        if (!strncmp(p->name, keep[i].name, n)) return keep[i].why;
        if (strstr(cmd, keep[i].name)) return keep[i].why;   /* e.g. "/bin/sh /usr/local/bin/nzeus-deck start" */
    }
    return NULL;
}

#ifdef NZ_TESTING
static int allowed[16], allowed_count, fake_self;
void procs_test_allow(int pid) { if (allowed_count < 16) allowed[allowed_count++] = pid; }
void procs_test_set_self(int pid) { fake_self = pid; }
int procs_self(void) { return fake_self ? fake_self : (int)getpid(); }
static int test_allows(int pid)
{
    for (int i = 0; i < allowed_count; i++) if (allowed[i] == pid) return 1;
    return 0;
}
#else
int procs_self(void) { return (int)getpid(); }
#endif

int procs_end(int pid, int force)
{
    /* never 0, -1 or 1: kill() would hit a whole group of programs, or init */
    if (pid <= 1) { errno = EINVAL; return -1; }
#ifdef NZ_TESTING
    if (!test_allows(pid)) { errno = EPERM; return -1; }    /* tests: only our own children */
#endif
    return kill(pid, force ? SIGKILL : SIGTERM);
}
