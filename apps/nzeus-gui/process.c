/* process.c - runs one program per Terminal session (see process.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "process.h"

/* Where programs are looked for, after whatever PATH NZEUS itself was given. */
#include "settings.h"
/* NZEUS's own helpers (python, nzeus-fetch...) are in NZ_DIR/bin (on the SD
 * Debian a link to /usr/local/lib/nzeus/deck-bin) */
#define EXTRA_PATH "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:" NZ_DIR "/bin"
#define READ_CHUNK      2048   /* bytes per read() */
#define CHUNKS_A_FRAME     4   /* at most 8 KB of output per frame, so `yes` can't freeze the screen */
#define STOP_WAIT_STEPS  100   /* process_shutdown: up to 100 x 2 ms for a polite stop */

/* ---------- small helpers ---------- */
static void fd_close(int *fd)
{
    if (*fd < 0) return;
    close(*fd);
    *fd = -1;
}

static void fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void signal_group(const Proc *p, int sig)
{
    if (p->group > 0) kill(-p->group, sig);   /* minus = the whole group, even its background jobs */
}

/* waitpid's status -> one number, like a shell's $? */
static int status_to_result(int status)
{
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    return -1;
}

/* Has the program ended? If so, note its result. Never waits. */
static bool reap(Proc *p)
{
    int status;
    if (p->pid <= 0 || waitpid(p->pid, &status, WNOHANG) != p->pid) return false;
    p->result = status_to_result(status);
    p->finished = true;
    p->pid = -1;              /* (the group is kept: a background job may still be in it) */
    fd_close(&p->to_child);   /* nobody is left to read typed input */
    return true;
}

/* ---------- starting ---------- */
void process_init(Proc *p)
{
    p->pid = p->group = -1;
    p->from_child = p->to_child = -1;
    p->finished = true;
    p->result = -1;
}

/* In the new child process: wire up the pipes and become the program. Never returns. */
static void become_program(int stdin_fd, int stdout_fd, char *const argv[], const char *cwd)
{
    setpgid(0, 0);                            /* a group of its own: Ctrl+C reaches all of it */
    if (cwd && cwd[0] && chdir(cwd) != 0) {}  /* (the folder is gone: start where we are) */

    char path[512];
    const char *old = getenv("PATH");
    snprintf(path, sizeof path, "%s%s" EXTRA_PATH, old && old[0] ? old : "", old && old[0] ? ":" : "");
    setenv("PATH", path, 1);

    dup2(stdin_fd, 0);
    dup2(stdout_fd, 1);
    dup2(stdout_fd, 2);                       /* errors show in the Terminal too */
    long top = sysconf(_SC_OPEN_MAX);         /* NZEUS's own files (screen, fonts, other  */
    for (int fd = 3; fd < (top > 0 ? top : 1024); fd++) close(fd);   /* sessions' pipes) stay ours */

    signal(SIGINT, SIG_DFL);                  /* normal signal handling for the program */
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    execvp(argv[0], argv);
    _exit(127);                               /* 127 = "command not found", as in a shell */
}

static bool launch(Proc *p, char *const argv[], const char *cwd)
{
    int out[2], in[2];                        /* [0] = read end, [1] = write end */
    /* Typing to a program that just ended would raise SIGPIPE and end NZEUS
     * itself: ignore it, write() then simply fails. */
    signal(SIGPIPE, SIG_IGN);
    if (pipe(out) != 0) return false;
    if (pipe(in) != 0) { close(out[0]); close(out[1]); return false; }

    pid_t pid = fork();
    if (pid == 0) become_program(in[0], out[1], argv, cwd);
    close(in[0]);                             /* (the child's ends) */
    close(out[1]);
    if (pid < 0) { close(in[1]); close(out[0]); return false; }

    setpgid(pid, pid);   /* also from this side, so the group exists before we could signal it */
    p->pid = p->group = pid;
    p->from_child = out[0];
    p->to_child = in[1];
    fd_nonblocking(p->from_child);            /* polling must never wait for output... */
    fd_nonblocking(p->to_child);              /* ...or for a program that doesn't read */
    p->finished = false;
    p->result = -1;
    return true;
}

bool process_start_command(Proc *p, const char *command, const char *cwd)
{
    char *argv[] = { "/bin/sh", "-c", (char *)command, NULL };
    return !process_is_running(p) && launch(p, argv, cwd);
}

bool process_start_script(Proc *p, const char *path, const char *cwd)
{
    char *argv[] = { "/bin/sh", (char *)path, NULL };
    return !process_is_running(p) && launch(p, argv, cwd);
}

/* argv = the arguments AFTER the program's own name (may be NULL) */
bool process_start_executable(Proc *p, const char *path, char *const argv[], const char *cwd)
{
    if (process_is_running(p)) return false;
    int count = 0;
    while (argv && argv[count]) count++;
    char **full = malloc(((size_t)count + 2) * sizeof *full);   /* name + arguments + NULL */
    if (!full) return false;
    full[0] = (char *)path;
    for (int i = 0; i < count; i++) full[i + 1] = argv[i];
    full[count + 1] = NULL;
    bool ok = launch(p, full, cwd);
    free(full);
    return ok;
}

/* ---------- while it runs ---------- */
bool process_is_running(const Proc *p) { return p->pid > 0 && !p->finished; }
int  process_exit_code(const Proc *p)  { return p->finished ? p->result : -1; }

void process_poll(Proc *p, process_output_cb on_output, void *ctx)
{
    reap(p);   /* (the result is known even if a background job still holds the output pipe) */
    char chunk[READ_CHUNK];
    for (int i = 0; i < CHUNKS_A_FRAME && p->from_child >= 0; i++) {
        ssize_t got = read(p->from_child, chunk, sizeof chunk);
        if (got > 0) {
            if (on_output) on_output(ctx, chunk, (int)got);
        } else {
            /* 0 = every writer is gone; an error other than "nothing yet": give up the pipe */
            if (got == 0 || (errno != EAGAIN && errno != EINTR)) fd_close(&p->from_child);
            break;
        }
    }
}

bool process_write_stdin(Proc *p, const char *buf, int len)
{
    if (!process_is_running(p) || p->to_child < 0) return false;
    int sent = 0;
    while (sent < len) {
        ssize_t n = write(p->to_child, buf + sent, (size_t)(len - sent));
        if (n > 0) sent += (int)n;
        else if (!(n < 0 && errno == EINTR)) return false;   /* full (not reading) or gone */
    }
    return true;
}

void process_close_stdin(Proc *p) { fd_close(&p->to_child); }

void process_interrupt(Proc *p)
{
    if (process_is_running(p)) signal_group(p, SIGINT);
}

void process_shutdown(Proc *p)
{
    if (p->group > 0) {
        signal_group(p, SIGTERM);                       /* ask nicely first... */
        for (int i = 0; i < STOP_WAIT_STEPS && !reap(p) && p->pid > 0; i++) usleep(2000);
        if (p->pid > 0) {                               /* ...then insist */
            signal_group(p, SIGKILL);
            usleep(10000);
            reap(p);
        }
        signal_group(p, SIGKILL);   /* anything left in the group (background jobs) goes too */
        p->group = -1;
    }
    p->pid = -1;
    p->finished = true;
    fd_close(&p->from_child);
    fd_close(&p->to_child);
}
