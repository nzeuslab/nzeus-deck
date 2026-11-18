/* carlog.c - live car values to a CSV file (see carlog.h). */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "carlog.h"
#include "nzeus_tick.h"
#include "obd.h"
#include "settings.h"

#define ROW_MS   500                  /* a row every half second */
#define FLUSH_MS 3000                 /* to the card every 3 seconds */

static struct {
    FILE *f;
    int pids[PID_COUNT], count, rows;
    uint32_t start, last_row, last_flush;
    char path[160];
} G;

int carlog_active(void) { return G.f != NULL; }
int carlog_rows(void) { return G.rows; }
const char *carlog_path(void) { return G.path; }

int carlog_start(const char *folder, const int *pids, int count)
{
    carlog_stop();
    char when[32];
    time_t now = time(NULL);
    mkdir(NZ_DIR, 0755);
    mkdir(folder, 0755);
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(G.path, sizeof G.path, "%s/log-%s.csv", folder, when);
    G.f = fopen(G.path, "w");
    if (!G.f) return -1;
    G.count = 0;
    for (int i = 0; i < count && G.count < PID_COUNT; i++) G.pids[G.count++] = pids[i];
    fprintf(G.f, "time_s");                               /* the first line: column names with units */
    for (int i = 0; i < G.count; i++) fprintf(G.f, ",%s (%s)", obd_pid_short(G.pids[i]), obd_pid_unit(G.pids[i]));
    fprintf(G.f, "\n");
    G.rows = 0;
    G.start = G.last_flush = nzeus_tick_ms();
    G.last_row = G.start - ROW_MS;                        /* the first row straight away */
    return 0;
}

void carlog_tick(void)
{
    if (!G.f) return;
    uint32_t now = nzeus_tick_ms();
    if (now - G.last_row < ROW_MS) return;
    G.last_row = now;
    const ObdLive *L = obd_live();
    fprintf(G.f, "%.1f", (now - G.start) / 1000.0);
    for (int i = 0; i < G.count; i++) {
        int p = G.pids[i];
        if (L->have[p]) fprintf(G.f, ",%.2f", L->value[p]);
        else fprintf(G.f, ",");                           /* no value yet: an empty cell */
    }
    fprintf(G.f, "\n");
    G.rows++;
    if (now - G.last_flush >= FLUSH_MS) {                 /* power-loss safe: really on the card */
        fflush(G.f);
        fsync(fileno(G.f));
        G.last_flush = now;
    }
}

void carlog_stop(void)
{
    if (!G.f) return;
    fflush(G.f);
    fsync(fileno(G.f));
    fclose(G.f);
    G.f = NULL;
}
