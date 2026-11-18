#ifndef NZEUS_BGJOB_H
#define NZEUS_BGJOB_H

/* Runs a shell command in the background (so the screen never freezes while
 * it works) and collects what it prints into a file. Poll it every frame. */
typedef struct {
    int pid;              /* 0 = not running */
    int exit_code;        /* after it finished (-1 = still running / never ran) */
    char out_path[96];    /* everything it printed */
} BgJob;

int  bgjob_start(BgJob *j, const char *command, const char *out_path);   /* 0 = started */
int  bgjob_poll(BgJob *j);         /* 1 = still running, 0 = finished (or idle) */
int  bgjob_running(const BgJob *j);
void bgjob_read(const BgJob *j, char *buf, int size);   /* its output so far (the last part if long) */
void bgjob_stop(BgJob *j);

#endif
