/* bt.c - Bluetooth keyboards and mice: runs nzeus-bt in the background (see bt.h). */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bgjob.h"
#include "bt.h"
#include "nzeus_tick.h"
#include "settings.h"

#define BT_BIN    NZ_DIR "/bin/nzeus-bt"
#define PAIR_FIFO "/tmp/nzeus-bt-pair.in"      /* the pairing program's input (see bt_pair) */
#define MAX_MINE  8
#define MAX_FOUND 12

static BgJob on_job, off_job, poll_job, scan_job, cmd_job, pair_job;
static int state = BT_CHECKING, poll_now = 1;
static uint32_t last_poll, scan_t0;
static char problem[120];
static BtDev mine[MAX_MINE], found[MAX_FOUND];
static int mine_n, found_n, list_ver, scan_ver;
static int cmd_finished;                         /* 1 = a command ended; its result is not collected yet */
static char cmd_verb[12];
static int pair_fd = -1;                         /* our end of PAIR_FIFO */
static long pair_off;                            /* how much of pair's output we have read */
static int pair_over = 1, pair_told_end;         /* told_end: DONE or ERROR was already given */

/* ---------- small helpers ---------- */
static void copy_text(char *dst, int n, const char *src)
{
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* the last line of `text` that has something on it (changes text) */
static const char *last_line(char *text)
{
    char *end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    char *s = end;
    while (s > text && s[-1] != '\n') s--;
    return s;
}

/* the start of a program's output file (not the end: scan lists the strongest first) */
static void read_start(const BgJob *j, char *buf, int n)
{
    FILE *f = fopen(j->out_path, "r");
    size_t got = 0;
    if (f) { got = fread(buf, 1, (size_t)n - 1, f); fclose(f); }
    buf[got] = 0;
}

/* "error pairing failed: x" -> "Pairing failed: x"; the usual ones in plainer words */
static void plain_error(const char *line, char *out, int n)
{
    if (!strncmp(line, "error ", 6)) line += 6;
    if (strstr(line, "not loaded") || strstr(line, "Bluetooth is off"))
        copy_text(out, n, "Bluetooth is off. Switch it on first.");
    else if (strstr(line, "no Bluetooth controller"))
        copy_text(out, n, "No Bluetooth dongle found. Plug it in, then try again.");
    else {
        copy_text(out, n, line);
        out[0] = (char)toupper((unsigned char)out[0]);
    }
}

int bt_valid_addr(const char *s)                 /* exactly AA:BB:CC:DD:EE:FF (safe in a command) */
{
    if (!s || strlen(s) != 17) return 0;
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* scan: ADDRESS NAME KIND TYPE RSSI SUPPORTED     list: ADDRESS NAME KIND TYPE connected
 * (the parts are separated by tabs) */
int bt_parse_line(const char *line, BtDev *d, int from_scan)
{
    char copy[200], *part[6], *p = copy;
    int n = 0;
    copy_text(copy, sizeof copy, line);
    copy[strcspn(copy, "\r\n")] = 0;
    while (n < 6) {                              /* cut at the tabs */
        part[n++] = p;
        p = strchr(p, '\t');
        if (!p) break;
        *p++ = 0;
    }
    if (n != (from_scan ? 6 : 5) || !bt_valid_addr(part[0])) return -1;
    memset(d, 0, sizeof *d);
    copy_text(d->addr, sizeof d->addr, part[0]);
    copy_text(d->name, sizeof d->name, part[1][0] && strcmp(part[1], "(no name)") ? part[1] : part[0]);
    copy_text(d->kind, sizeof d->kind, part[2]);
    copy_text(d->type, sizeof d->type, part[3]);
    if (from_scan) {
        d->rssi = atoi(part[4]);
        if (d->rssi == 127) d->rssi = 0;         /* 127 = not known */
        d->supported = !strcmp(part[5], "yes");
    } else {
        d->connected = !strcmp(part[4], "connected");
    }
    return 0;
}

static int is_mine(const char *addr)
{
    for (int i = 0; i < mine_n; i++) if (!strcmp(mine[i].addr, addr)) return 1;
    return 0;
}

/* ---------- state: "nzeus-bt status; nzeus-bt list" every 3 s ---------- */
static void read_poll_answer(void)
{
    char buf[1400];
    BtDev now[MAX_MINE];
    int loaded = -1, controller = 1, powered = 0, n = 0;
    read_start(&poll_job, buf, sizeof buf);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "loaded=", 7)) loaded = !strncmp(line + 7, "yes", 3);
        else if (!strncmp(line, "controller=", 11)) controller = strncmp(line + 11, "none", 4) != 0;
        else if (!strncmp(line, "powered=", 8)) powered = !strncmp(line + 8, "yes", 3);
        else if (n < MAX_MINE && bt_parse_line(line, &now[n], 0) == 0) n++;
    }
    if (loaded < 0) state = BT_MISSING;          /* no answer at all: nzeus-bt is not there */
    else if (!loaded) state = BT_OFF;
    else if (!controller) state = BT_NO_DONGLE;
    else state = powered ? BT_ON : BT_OFF;
    if (n != mine_n || memcmp(now, mine, (size_t)n * sizeof now[0])) {
        memcpy(mine, now, (size_t)n * sizeof now[0]);
        mine_n = n;
        list_ver++;
    }
}

static void read_scan_answer(void)
{
    char buf[1400];
    BtDev d;
    read_start(&scan_job, buf, sizeof buf);
    found_n = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "error ", 6)) { plain_error(line, problem, sizeof problem); continue; }
        if (found_n < MAX_FOUND && bt_parse_line(line, &d, 1) == 0 && !is_mine(d.addr)) found[found_n++] = d;
    }
    scan_ver++;
}

void bt_poll(void)
{
    if (bgjob_running(&on_job) && bgjob_poll(&on_job) == 0) {     /* nzeus-bt-up.sh finished */
        char log[600];
        bgjob_read(&on_job, log, sizeof log);
        if (on_job.exit_code != 0) snprintf(problem, sizeof problem, "Could not switch on: %.96s", last_line(log));
        state = BT_CHECKING;
        poll_now = 1;
    }
    if (bgjob_running(&off_job) && bgjob_poll(&off_job) == 0) {   /* nzeus-bt off finished */
        char log[300];
        bgjob_read(&off_job, log, sizeof log);
        if (off_job.exit_code != 0) plain_error(last_line(log), problem, sizeof problem);
        state = BT_CHECKING;
        poll_now = 1;
    }
    if (bgjob_running(&poll_job) && bgjob_poll(&poll_job) == 0) read_poll_answer();
    if (bgjob_running(&scan_job) && bgjob_poll(&scan_job) == 0) read_scan_answer();
    if (bgjob_running(&cmd_job) && bgjob_poll(&cmd_job) == 0) { cmd_finished = 1; poll_now = 1; }

    uint32_t now = nzeus_tick_ms();
    if (!bgjob_running(&poll_job) && !bgjob_running(&on_job) && !bgjob_running(&off_job) &&
        (poll_now || now - last_poll > 3000)) {
        poll_now = 0;
        last_poll = now;
        bgjob_start(&poll_job, BT_BIN " status; " BT_BIN " list", "/tmp/nzeus-bt-poll.log");
    }
}

void bt_refresh(void) { poll_now = 1; }

int bt_state(void)
{
    if (bgjob_running(&on_job)) return BT_STARTING;
    if (bgjob_running(&off_job)) return BT_STOPPING;
    return state;
}

const char *bt_problem(void) { return problem; }

const char *bt_start_line(void)
{
    static char log[400];
    bgjob_read(&on_job, log, sizeof log);
    return last_line(log);
}

/* ---------- on / off ---------- */
/* Bluetooth's on/off is remembered in this file. (The factory system's
 * start.sh switched it on again at power-on; on the SD Debian nothing reads
 * it yet - a start-up script could, then paired devices reconnect by themselves.) */
#define BT_AT_START NZ_DIR "/bluetooth/on-at-start"

int bt_turn_on(void)
{
    if (bgjob_running(&on_job) || bgjob_running(&off_job)) return -1;
    problem[0] = 0;
    mkdir(NZ_DIR "/bluetooth", 0755);
    FILE *f = fopen(BT_AT_START, "w");
    if (f) fclose(f);
    /* the log and the kernel's messages go to the SD card (nzeus/tests), so a
     * failure can still be read on the PC after the console is switched off */
    mkdir(NZ_DIR "/tests", 0755);
    return bgjob_start(&on_job, "NZEUS_DIR=" NZ_DIR " sh " NZ_DIR "/bin/nzeus-bt-up.sh; rc=$?; "
                       "dmesg > " NZ_DIR "/tests/bt-dmesg.log 2>&1; sync; exit $rc",
                       NZ_DIR "/tests/bt-up.log");
}

int bt_turn_off(void)
{
    if (bgjob_running(&on_job) || bgjob_running(&off_job)) return -1;
    problem[0] = 0;
    remove(BT_AT_START);                            /* (stays off after the next power-on) */
    return bgjob_start(&off_job, BT_BIN " off", "/tmp/nzeus-bt-off.log");
}

/* ---------- my devices ---------- */
int bt_device_count(void) { return mine_n; }
const BtDev *bt_device(int i) { return i >= 0 && i < mine_n ? &mine[i] : NULL; }
int bt_list_version(void) { return list_ver; }

/* ---------- finding devices ---------- */
int bt_scan_start(int seconds)
{
    char cmd[160];
    if (bgjob_running(&scan_job)) return 0;      /* already searching */
    problem[0] = 0;
    snprintf(cmd, sizeof cmd, BT_BIN " scan %d", seconds);
    scan_t0 = nzeus_tick_ms();
    return bgjob_start(&scan_job, cmd, "/tmp/nzeus-bt-scan.log");
}
int bt_scanning(void) { return bgjob_running(&scan_job); }
int bt_scan_elapsed_ms(void) { return (int)(nzeus_tick_ms() - scan_t0); }
int bt_found_count(void) { return found_n; }
const BtDev *bt_found(int i) { return i >= 0 && i < found_n ? &found[i] : NULL; }
int bt_scan_version(void) { return scan_ver; }

/* ---------- connect / disconnect / forget ---------- */
int bt_command(const char *verb, const char *addr)
{
    char cmd[160];
    if (bgjob_running(&cmd_job) || !bt_valid_addr(addr)) return -1;
    if (strcmp(verb, "connect") && strcmp(verb, "disconnect") && strcmp(verb, "remove")) return -1;
    copy_text(cmd_verb, sizeof cmd_verb, verb);
    cmd_finished = 0;
    snprintf(cmd, sizeof cmd, BT_BIN " %s %s", verb, addr);
    return bgjob_start(&cmd_job, cmd, "/tmp/nzeus-bt-cmd.log");
}

int bt_command_busy(void) { return bgjob_running(&cmd_job); }

int bt_command_result(char *msg, int n)
{
    char buf[400];
    if (!cmd_finished) return 0;
    cmd_finished = 0;
    bgjob_read(&cmd_job, buf, sizeof buf);
    const char *l = last_line(buf);
    if (cmd_job.exit_code != 0 || !strncmp(l, "error ", 6)) plain_error(l[0] ? l : "it did not work", msg, n);
    else if (!strcmp(cmd_verb, "connect")) copy_text(msg, n, strncmp(l, "connecting", 10) ? "Connected" : "Connecting...");
    else if (!strcmp(cmd_verb, "disconnect")) copy_text(msg, n, "Disconnected");
    else copy_text(msg, n, "Forgotten");
    return 1;
}

/* ---------- pairing ---------- */
static void close_fifo(void)
{
    if (pair_fd >= 0) close(pair_fd);
    pair_fd = -1;
    unlink(PAIR_FIFO);
}

/* The pairing program sometimes needs a number typed in (its input, stdin).
 * bgjob gives its programs no input, so we use a named pipe (a FIFO, a "pipe
 * with a file name"): the program's input comes from it, and we write the
 * number into it. We open it for reading AND writing ourselves first, so
 * neither side ever waits for the other one to open it. */
int bt_pair(const char *addr)
{
    char cmd[200];
    if (bgjob_running(&pair_job) || !bt_valid_addr(addr)) return -1;
    close_fifo();
    if (mkfifo(PAIR_FIFO, 0600) != 0) return -1;
    pair_fd = open(PAIR_FIFO, O_RDWR | O_NONBLOCK);
    if (pair_fd < 0) { close_fifo(); return -1; }
    fcntl(pair_fd, F_SETFD, FD_CLOEXEC);         /* programs we start don't get our end */
    snprintf(cmd, sizeof cmd, "exec " BT_BIN " pair %s < " PAIR_FIFO, addr);
    if (bgjob_start(&pair_job, cmd, "/tmp/nzeus-bt-pair.log") != 0) { close_fifo(); return -1; }
    pair_off = 0;
    pair_over = pair_told_end = 0;
    return 0;
}

int bt_pairing(void) { return bgjob_running(&pair_job); }

int bt_pair_answer(long number)
{
    char line[16];
    if (pair_fd < 0 || number < 0 || number > 999999) return -1;
    int n = snprintf(line, sizeof line, "%ld\n", number);
    return write(pair_fd, line, (size_t)n) == n ? 0 : -1;
}

void bt_pair_cancel(void)
{
    bgjob_stop(&pair_job);
    close_fifo();
    pair_over = 1;
}

/* the next whole line of pair's output (a half-written line waits, unless it has ended) */
static int next_line(char *out, int n, int ended)
{
    FILE *f = fopen(pair_job.out_path, "r");
    int got = 0;
    out[0] = 0;
    if (!f) return 0;
    fseek(f, pair_off, SEEK_SET);
    if (fgets(out, n, f)) {
        size_t len = strlen(out);
        if (out[len - 1] == '\n' || ended || len == (size_t)n - 1) { pair_off += (long)len; got = 1; }
    }
    fclose(f);
    if (!got) out[0] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return got;
}

/* one line of `nzeus-bt pair` -> BT_EV_* (README: "pair progress lines") */
static int pair_line(const char *line, char *text, int n)
{
    char digits[20] = "";
    if (!strncmp(line, "passkey ", 8) || (!strncmp(line, "pin ", 4) && strstr(line, "type it"))) {
        sscanf(strchr(line, ' ') + 1, "%19[0-9]", digits);
        copy_text(text, n, digits);
        return BT_EV_PASSKEY;
    }
    if (!strncmp(line, "typed ", 6)) { copy_text(text, n, line + 6); return BT_EV_TYPED; }
    if (!strncmp(line, "enter-passkey", 13)) return BT_EV_ENTER;
    if (!strcmp(line, "paired")) return BT_EV_PAIRED;
    if (!strncmp(line, "connected", 9)) { pair_told_end = 1; copy_text(text, n, "Connected"); return BT_EV_DONE; }
    if (!strncmp(line, "connecting", 10)) { pair_told_end = 1; copy_text(text, n, "Connecting..."); return BT_EV_DONE; }
    if (!strncmp(line, "error ", 6)) { pair_told_end = 1; plain_error(line, text, n); return BT_EV_ERROR; }
    if (!strncmp(line, "pin ", 4)) { copy_text(text, n, "Using the usual PIN for devices without keys"); return BT_EV_INFO; }
    if (!strncmp(line, "warning ", 8)) { plain_error(line + 8, text, n); return BT_EV_INFO; }
    if (!strncmp(line, "confirm", 7)) { copy_text(text, n, "Accepted the device"); return BT_EV_INFO; }
    if (line[0]) { plain_error(line, text, n); return BT_EV_INFO; }   /* e.g. "pairing ... (classic)..." */
    return BT_EV_NONE;
}

int bt_pair_next(char *text, int n)
{
    char line[160];
    text[0] = 0;
    if (pair_over) return BT_EV_NONE;
    int running = bgjob_poll(&pair_job);         /* (first: then no line is missed at the end) */
    while (next_line(line, sizeof line, !running)) {
        int ev = pair_line(line, text, n);
        if (ev != BT_EV_NONE) return ev;
    }
    if (running) return BT_EV_NONE;
    pair_over = 1;                               /* the program has ended */
    close_fifo();
    if (pair_told_end) return BT_EV_NONE;
    if (pair_job.exit_code == 0) { copy_text(text, n, "Connected"); return BT_EV_DONE; }
    copy_text(text, n, "Pairing stopped. Is the device still in pairing mode?");
    return BT_EV_ERROR;
}
