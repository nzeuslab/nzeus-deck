/* bgjob.c - background shell commands (see bgjob.h). */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bgjob.h"

int bgjob_start(BgJob *j, const char *command, const char *out_path)
{
    if (j->pid) return -1;                           /* one job at a time per BgJob */
    snprintf(j->out_path, sizeof j->out_path, "%s", out_path);
    j->exit_code = -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {                                  /* the child: output to the file, then the shell */
        setpgid(0, 0);
        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) { dup2(null, 0); close(null); }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    j->pid = (int)pid;
    return 0;
}

int bgjob_poll(BgJob *j)
{
    if (!j->pid) return 0;
    int status;
    pid_t r = waitpid((pid_t)j->pid, &status, WNOHANG);
    if (r == 0) return 1;                            /* still working */
    j->exit_code = (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
    j->pid = 0;
    return 0;
}

int bgjob_running(const BgJob *j) { return j->pid != 0; }

void bgjob_read(const BgJob *j, char *buf, int size)
{
    buf[0] = 0;
    FILE *f = fopen(j->out_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f), from = n > size - 1 ? n - (size - 1) : 0;
    fseek(f, from, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)size - 1, f);
    buf[got] = 0;
    fclose(f);
}

void bgjob_stop(BgJob *j)
{
    if (!j->pid) return;
    kill(-(pid_t)j->pid, SIGTERM);                   /* the whole group (the shell and its children) */
    waitpid((pid_t)j->pid, NULL, 0);
    j->pid = 0;
}
