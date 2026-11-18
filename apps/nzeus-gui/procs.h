#ifndef NZEUS_PROCS_H
#define NZEUS_PROCS_H

/* Reading the running programs ("processes") for the Task Manager.
 *
 * Linux shows every running program as a folder in /proc, named after its
 * number (the PID, "process ID"): /proc/123/status has its name, state,
 * parent and memory. A few files in /proc itself tell about the whole
 * system (meminfo, loadavg, stat, uptime).
 *
 * Everything is read from a "proc root" that is /proc normally; the tests
 * point it at a fake folder so they can check the code with made-up programs. */

#define PROCS_NAME_LEN 32

typedef struct {
    int  pid, ppid;              /* its number, and its parent's number */
    char name[PROCS_NAME_LEN];   /* e.g. "nzeus-modern" (Linux keeps at most 15 letters) */
    char state;                  /* 'R' running, 'S' sleeping, 'Z' finished... */
    long rss_kb;                 /* memory it really uses (RAM), in kB */
    int  kernel;                 /* 1 = a kernel thread: part of Linux itself */
} ProcInfo;

typedef struct {
    long mem_total_kb, mem_avail_kb;   /* RAM: all of it, and what is still free to use */
    long swap_total_kb, swap_free_kb;  /* swap: memory borrowed from the SD card */
    double load1;                      /* programs wanting the CPU (1-minute average) */
    double uptime_s;                   /* seconds since the console started */
} ProcSummary;

void procs_set_root(const char *root);          /* "/proc" (the default), or a test folder */
const char *procs_root(void);

int  procs_read(int pid, ProcInfo *p);          /* 0 = read it, -1 = no such program */
int  procs_list(ProcInfo *out, int max);        /* all programs, biggest memory first; returns how many */
int  procs_summary(ProcSummary *s);             /* 0 = ok */
int  procs_cpu_times(unsigned long long *busy, unsigned long long *total);  /* from "stat"; 0 = ok */
int  procs_alive(int pid);                      /* 1 = still running (a finished "zombie" counts as gone) */
const char *procs_state_word(char state);       /* 'S' -> "sleeping" */

/* Why this program must not be ended, in plain words, or NULL if it may be.
 * self_pid: NZEUS's own PID (getpid(); tests pass a fake one).
 * Refused: PID 1 (init), kernel threads, NZEUS itself and the programs that
 * started it, and the system's own helpers (nzeus-deck, the SSH server,
 * the USB rescue shell). */
const char *procs_protected(const ProcInfo *p, int self_pid);

/* Ask a program to stop (SIGTERM), or force it (force = 1: SIGKILL).
 * 0 = signal sent. On the PC test build only PIDs added with
 * procs_test_allow() can be signalled, so a test can never end a real program. */
int  procs_end(int pid, int force);
int  procs_self(void);                          /* NZEUS's own PID (getpid()) */
#ifdef NZ_TESTING
void procs_test_allow(int pid);                 /* this PID may be signalled */
void procs_test_set_self(int pid);              /* pretend NZEUS is this PID (0 = the real one) */
#endif

#endif
