#ifndef NZ_PROCESS_H
#define NZ_PROCESS_H

#include <stdbool.h>
#include <sys/types.h>

/* process.c - runs one program for a Terminal session, and never waits for it.
 *
 * The program gets a process group of its own (so Ctrl+C reaches everything it
 * started too) and two pipes: one carries what it prints back to us, the other
 * carries what the user types to it. The console's kernel has no
 * pseudo-terminals, so pipes are the only way to talk to a program.
 *
 * Every frame the Terminal calls process_poll(), which collects whatever the
 * program printed since the last frame. Nothing in here ever blocks. */

typedef struct {
    pid_t pid;         /* the program, or -1 when there is none */
    pid_t group;       /* its process group: still valid after the program ended */
    int   from_child;  /* we read here: the program's stdout and stderr */
    int   to_child;    /* we write here: the program's stdin */
    bool  finished;    /* true = no program is running */
    int   result;      /* its exit code (128 + N if signal N stopped it), -1 = not known yet */
} Proc;

/* the function process_poll() hands the program's output to; ctx is passed through */
typedef void (*process_output_cb)(void *ctx, const char *buf, int len);

void process_init(Proc *p);     /* once, before the first use */

/* Start a program in folder cwd. False if one is still running or it could not start. */
bool process_start_command(Proc *p, const char *command, const char *cwd);   /* /bin/sh -c COMMAND */
bool process_start_script(Proc *p, const char *path, const char *cwd);       /* /bin/sh PATH */
bool process_start_executable(Proc *p, const char *path, char *const argv[], const char *cwd);

void process_poll(Proc *p, process_output_cb on_output, void *ctx);   /* every frame */
void process_interrupt(Proc *p);   /* Ctrl+C: SIGINT to the program's whole group */
void process_shutdown(Proc *p);    /* stop it for good (TERM, then KILL) and close everything */

/* Typed input for the running program. Never blocks: false when nothing runs,
 * its input was closed, or it isn't reading (the pipe is full). */
bool process_write_stdin(Proc *p, const char *buf, int len);
void process_close_stdin(Proc *p);   /* Ctrl+D: the program sees the end of its input */

bool process_is_running(const Proc *p);
int  process_exit_code(const Proc *p);   /* of the program that finished last; -1 while one runs */

#endif
