/* test_tools.c - tests the four tool apps (Task Manager, Calculator, Notes,
 * Clock) on the PC. Built and run by R36S/build_tools_test.sh.
 *
 *   1. unit checks: the Calculator's maths (calc_eval.c), the process
 *      reading and protection (procs.c, on a fake /proc made in /tmp), and
 *      the Notes file names
 *   2. the apps themselves: opened in NZEUS, driven with simulated buttons,
 *      keyboard and mouse, results checked
 *   3. memory: each app opened and closed 50 times must not keep any memory
 *   4. pictures of every app in three themes, full-screen and in a window
 *
 * Every check prints PASS or FAIL; the exit code is the number of FAILs.
 * Usage: test_tools SHOTS_FOLDER        (all of it)
 *        test_tools --unit              (only part 1: quick, for the mutants)
 * Built with -DNZ_TESTING -DNZ_SDCARD="/tmp/tools_sd" (a fake SD card).
 * Safety: the Task Manager can only signal PIDs the test allowed (procs.c),
 * and the test allows only the two child processes it starts itself. */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "calc_eval.h"
#include "devtools.h"
#include "display.h"
#include "health.h"
#include "home.h"
#include "wifiinfo.h"
#include "btinfo.h"
#include "hw.h"
#include "nzeus_tick.h"
#include "nzui.h"
#include "procs.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

extern NzApp app_taskmgr, app_calculator, app_notes, app_clock;
/* test hooks in the apps */
int app_taskmgr_count(void);
int app_taskmgr_selected_pid(void);
int app_taskmgr_select_pid(int pid);
void app_taskmgr_refresh(void);
const char *app_calculator_expr(void);
const char *app_calculator_top(void);
int app_calculator_history_count(void);
const char *app_calculator_focused(void);
lv_obj_t *app_calculator_key_obj(const char *label);
void notes_safe_name(const char *title, time_t when, char *out, size_t n);
int notes_new_path(const char *dir, const char *title, time_t when, char *out, size_t n);
int app_notes_count(void);
const char *app_notes_path(int i);
const char *app_notes_title(int i);
int app_clock_page(void);
const char *app_clock_text(int which);
int app_clock_lap_count(void);
int app_clock_overlay_shown(void);
void app_clock_timer_skip(uint32_t ms);
void app_clock_stopwatch_skip(uint32_t ms);
const char *app_editor_path(void);              /* app_editor.c */
const char *app_devtools_out(void);             /* app_devtools.c */
int app_devtools_tool(void);
const char *app_devtools_in(void);
extern NzApp app_devtools;
const char *app_health_verdict(void);           /* app_health.c */
const char *app_health_value(int row);
extern NzApp app_health;

#define FAKE_PROC "/tmp/tools_proc"
#define NOTES     NZ_DIR "/notes"

static int fails, passes;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passes++; else fails++;
}

/* ================= 1. unit checks ================= */

/* ---- the Dev toolbox's workers (devtools.c) ----
 * dt_has: the tool answered rc (0 ok / -1 bad input) and its answer contains want */
typedef int (*DtFn)(const char *in, char *out, size_t n);
static void dt_has(DtFn f, const char *name, const char *in, int rc_want, const char *want)
{
    char out[2048], what[400];
    int rc = f(in, out, sizeof out);
    snprintf(what, sizeof what, "devtools %s \"%.40s\" -> %s\"%.60s\"  (got %d: \"%.80s\")", name, in,
             rc_want ? "error " : "", want, rc, out);
    check(rc == rc_want && strstr(out, want) != NULL, what);
}
static void dt_is(DtFn f, const char *name, const char *in, const char *want)   /* exactly this answer */
{
    char out[2048], what[400];
    int rc = f(in, out, sizeof out);
    snprintf(what, sizeof what, "devtools %s \"%.40s\" = \"%.60s\"  (got \"%.80s\")", name, in, want, out);
    check(rc == 0 && !strcmp(out, want), what);
}
static int colour_fn(const char *in, char *out, size_t n) { return dt_colour(in, out, n, NULL); }
static time_t time_now;
static int time_fn(const char *in, char *out, size_t n) { return dt_time(in, time_now, out, n); }
static const char *regex_text;
static int regex_fn(const char *in, char *out, size_t n) { return dt_regex(in, regex_text, out, n); }

static void test_devtools(void)
{
    printf("--- devtools ---\n");
    dt_has(dt_number, "number", "255", 0, "Hex       0xFF\nBinary    1111 1111\nOctal     0377");
    dt_has(dt_number, "number", "0b1010", 0, "Decimal   10\n");
    dt_has(dt_number, "number", "0o17", 0, "Decimal   15\n");
    dt_has(dt_number, "number", "'A'", 0, "Decimal   65\n");
    dt_has(dt_number, "number", "65", 0, "Character 'A'");
    dt_has(dt_number, "number", "1_000", 0, "Decimal   1000\n");
    dt_has(dt_number, "number", "-1", 0, "Decimal   -1\nHex       0xFFFFFFFF  (32-bit)");
    dt_has(dt_number, "number", "0x1G", -1, "Hex uses only");
    dt_has(dt_number, "number", "0b102", -1, "Binary uses only 0 and 1");
    dt_has(dt_number, "number", "", -1, "Type a number");
    dt_has(dt_number, "number", "99999999999999999999999", -1, "Too big");
    dt_is(dt_b64_encode, "base64", "Hello", "SGVsbG8=");
    dt_is(dt_b64_encode, "base64", "Hi", "SGk=");
    dt_is(dt_b64_encode, "base64", "Man", "TWFu");
    dt_is(dt_b64_encode, "base64", "", "");
    dt_is(dt_b64_decode, "un-base64", "SGVsbG8=", "Hello");
    dt_is(dt_b64_decode, "un-base64", "SGk", "Hi");                 /* (padding left off: fine) */
    dt_has(dt_b64_decode, "un-base64", "AAE=", 0, "(not text: 2 bytes)\n00 01");
    dt_has(dt_b64_decode, "un-base64", "S", -1, "length is wrong");
    dt_has(dt_b64_decode, "un-base64", "SG@k", -1, "Not Base64");
    dt_has(dt_b64_decode, "un-base64", "SG=k", -1, "= can only be at the end");
    dt_is(dt_hex_encode, "hex", "Hi!", "48 69 21");
    dt_is(dt_hex_decode, "un-hex", "48:69 0x21", "Hi!");
    dt_is(dt_hex_decode, "un-hex", "486921", "Hi!");
    dt_has(dt_hex_decode, "un-hex", "486", -1, "one digit is missing");
    dt_has(dt_hex_decode, "un-hex", "4G", -1, "Not hex");
    dt_is(dt_url_encode, "url", "a b&c=d", "a%20b%26c%3Dd");
    dt_is(dt_url_encode, "url", "safe-_.~", "safe-_.~");
    dt_is(dt_url_decode, "un-url", "a%20b+c", "a b c");
    dt_has(dt_url_decode, "un-url", "50%", -1, "2 hex digits");
    check(dt_crc32("123456789", 9) == 0xCBF43926u, "devtools CRC-32 of \"123456789\" = 0xCBF43926 (the standard check value)");
    dt_has(dt_hash, "hash", "abc", 0, "SHA-256\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nCRC-32   0x352441C2\nLength   3 bytes");
    time_now = 0;
    dt_has(time_fn, "time", "0", 0, "UTC       1970-01-01 00:00:00 (Thursday)");
    dt_has(time_fn, "time", "2026-10-07", 0, "Epoch     1791331200\n");
    dt_has(time_fn, "time", "2026-10-07 14:30", 0, "Epoch     1791383400\n");
    dt_has(time_fn, "time", "2026-10-07T14:30:00", 0, "Epoch     1791383400\n");
    dt_has(time_fn, "time", "1700000000000", 0, "Epoch     1700000000  (you typed milliseconds)\nUTC       2023-11-14 22:13:20 (Tuesday)");
    dt_has(time_fn, "time", "100", 0, "From now  in 1 minute");
    time_now = 1791331200 + 3 * 86400;
    dt_has(time_fn, "time", "2026-10-07", 0, "From now  3 days ago");
    dt_has(time_fn, "time", "", 0, "Epoch     1791590400\n");             /* empty = now */
    dt_has(time_fn, "time", "2026-13-01", -1, "doesn't exist");
    dt_has(time_fn, "time", "hello", -1, "Type an epoch");
    dt_has(colour_fn, "colour", "#FF8000", 0, "Hex      #FF8000\nRGB      255, 128, 0\nRGB565   0xFC00");
    dt_has(colour_fn, "colour", "ff8000", 0, "RGB      255, 128, 0");
    dt_has(colour_fn, "colour", "#F80", 0, "Hex      #FF8800");
    dt_has(colour_fn, "colour", "0, 255, 0", 0, "Hex      #00FF00");
    dt_has(colour_fn, "colour", "565:F800", 0, "Hex      #FF0000");
    dt_has(colour_fn, "colour", "0x07E0", 0, "Hex      #00FF00");
    dt_has(colour_fn, "colour", "#GG0000", -1, "Type #FF8000");
    dt_has(colour_fn, "colour", "300,0,0", -1, "0 to 255");
    { uint32_t c = 0; char o[256]; dt_colour("#123456", o, sizeof o, &c); check(c == 0x123456, "devtools colour: gives the colour for the sample box"); }
    dt_is(dt_json, "json", "{\"a\":1,\"b\":[true,null]}", "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null\n  ]\n}");
    dt_is(dt_json, "json", " [ ] ", "[]");
    dt_is(dt_json, "json", "{\"t\":\"x\\\"y\",\"n\":-1.5e3}", "{\n  \"t\": \"x\\\"y\",\n  \"n\": -1.5e3\n}");
    dt_has(dt_json, "json", "{\"a\":1,}", -1, "a name in \" \" was expected\n(at character 8, line 1)");
    dt_has(dt_json, "json", "['a']", -1, "JSON uses \" for text");
    dt_has(dt_json, "json", "01", -1, "can't start with 0");
    dt_has(dt_json, "json", "[1 2]", -1, "a , or ] was expected");
    dt_has(dt_json, "json", "\"abc", -1, "not closed");
    dt_has(dt_json, "json", "1 2", -1, "more after the end");
    dt_has(dt_json, "json", "{\"a\":\n tru}", -1, "line 2");
    regex_text = "R36S has 2 SD slots, 42 MB";
    dt_is(regex_fn, "regex", "[0-9]+", "Match 1 at 1-3: \"36\"\nMatch 2 at 9-10: \"2\"\nMatch 3 at 21-23: \"42\"");
    regex_text = "mail alice@lab now";
    dt_is(regex_fn, "regex", "([a-z]+)@([a-z]+)", "Match 1 at 5-14: \"alice@lab\"\n   group 1: \"alice\"\n   group 2: \"lab\"");
    dt_is(regex_fn, "regex", "xyz", "No match");
    regex_text = "ab";
    dt_has(regex_fn, "regex", "x*", 0, "Match 3 at 2-2");                    /* empty matches: no endless loop */
    dt_has(regex_fn, "regex", "a(", -1, "The pattern has a mistake");
}

static void calc_ok(const char *expr, double want)
{
    double v = 12345;
    char err[80], what[200];
    int rc = calc_eval(expr, &v, err, sizeof err);
    if (rc) snprintf(what, sizeof what, "calc \"%.60s\" = %.10g  (got the error \"%s\")", expr, want, err);
    else snprintf(what, sizeof what, "calc \"%.60s\" = %.10g  (got %.10g)", expr, want, v);
    check(rc == 0 && fabs(v - want) <= 1e-9 * (fabs(want) + 1), what);
}

static void calc_err(const char *expr, const char *want)
{
    double v = 0;
    char err[80], what[200];
    int rc = calc_eval(expr, &v, err, sizeof err);
    snprintf(what, sizeof what, "calc \"%.60s\" -> error \"%s\"  (got \"%s\")", expr, want, rc ? err : "no error");
    check(rc == -1 && strstr(err, want) != NULL, what);
}

static void calc_fmt(double v, const char *want)
{
    char b[48], what[120];
    calc_format(v, b, sizeof b);
    snprintf(what, sizeof what, "format %.12g -> \"%s\" (got \"%s\")", v, want, b);
    check(!strcmp(b, want), what);
}

static void test_calc(void)
{
    printf("--- calc_eval ---\n");
    calc_ok("1+2", 3);
    calc_ok("2+3*4", 14);                    /* precedence */
    calc_ok("2*3+4", 10);
    calc_ok("(2+3)*4", 20);                  /* brackets */
    calc_ok("10/4", 2.5);
    calc_ok("2-3-4", -5);                    /* left to right */
    calc_ok("100/10/2", 5);
    calc_ok("-5+3", -2);                     /* unary minus */
    calc_ok("2*-3", -6);
    calc_ok("--4", 4);
    calc_ok("-(2+3)", -5);
    calc_ok("+7", 7);
    calc_ok("50%", 0.5);                     /* percent */
    calc_ok("200+10%", 220);
    calc_ok("200-10%", 180);
    calc_ok("200*10%", 20);
    calc_ok("2(3+1)", 8);                    /* a bracket right after a number */
    calc_ok(" 1 +  2 ", 3);
    calc_ok(".5+7.", 7.5);
    calc_ok("1.5e+3", 1500);
    calc_ok("0.1+0.2", 0.3);
    calc_ok("((((((((((1))))))))))", 1);
    calc_err("1/0", "Can't divide by zero");
    calc_err("5/(2-2)", "Can't divide by zero");
    calc_err("2+", "Something is missing");
    calc_err("(2+3", "Something is missing");
    calc_err("()", "Something is missing");
    calc_err("", "Something is missing");
    calc_err("*3", "Something is missing");
    calc_err(".", "Something is missing");
    calc_err("abc", "I don't understand \"a\"");
    calc_err("2+a", "I don't understand");
    calc_err("2)", "A ( is missing");
    calc_err("1.2.3", "two dots");
    calc_err("2 3", "A sign is missing");
    calc_err("1e400", "too big");
    calc_err("2e", "I don't understand \"e\"");

    char big[1200];                          /* very long input */
    for (int i = 0; i < 600; i++) { big[2 * i] = '1'; big[2 * i + 1] = '+'; }
    big[1199] = 0;
    calc_err(big, "too long");
    char deep[200];                          /* brackets too deep: refused, not a crash */
    memset(deep, '(', 40); deep[40] = '1'; memset(deep + 41, ')', 40); deep[81] = 0;
    calc_err(deep, "Too many brackets");
    memset(deep, '-', 100); deep[100] = '5'; deep[101] = 0;
    calc_err(deep, "Too many signs");
    memset(deep, '(', 30); deep[30] = '2'; memset(deep + 31, ')', 30); deep[61] = 0;
    calc_ok(deep, 2);

    int bad = 0;                             /* garbage: never a crash, always an answer or a message */
    const char *chars = "0123456789+-*/().% xe";
    srand(7);
    for (int t = 0; t < 20000; t++) {
        char s[64];
        int n = rand() % 60;
        for (int i = 0; i < n; i++) s[i] = chars[rand() % (int)strlen(chars)];
        s[n] = 0;
        double v;
        char err[80] = "x";
        int rc = calc_eval(s, &v, err, sizeof err);
        if (rc == 0 && !isfinite(v)) bad++;
        if (rc == -1 && !err[0]) bad++;
        if (rc != 0 && rc != -1) bad++;
    }
    check(bad == 0, "calc: 20000 random garbage inputs give a number or a message");

    calc_fmt(3, "3");
    calc_fmt(-12.25, "-12.25");
    calc_fmt(1.0 / 3, "0.3333333333");
    calc_fmt(-0.0, "0");
    calc_fmt(1e20, "1e+20");
    calc_fmt(0.1 + 0.2, "0.3");
}

/* ---------- a fake /proc ---------- */
static void write_file(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(text, 1, len, f);
    fclose(f);
}

/* one fake program: rss < 0 = no memory lines (a kernel thread); cmd: words split by '|' */
static void fake_proc(int pid, const char *name, char state, int ppid, long rss, const char *cmd)
{
    char dir[128], path[160], status[512], cl[256];
    snprintf(dir, sizeof dir, FAKE_PROC "/%d", pid);
    mkdir(dir, 0755);
    int n = snprintf(status, sizeof status, "Name:\t%s\nState:\t%c (x)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n", name, state, pid, pid, ppid);
    if (rss >= 0) snprintf(status + n, sizeof status - (size_t)n, "VmPeak:\t%ld kB\nVmSize:\t%ld kB\nVmRSS:\t%8ld kB\nThreads:\t1\n", rss * 2, rss * 2, rss);
    snprintf(path, sizeof path, "%s/status", dir);
    write_file(path, status, strlen(status));
    size_t len = 0;
    for (const char *c = cmd; *c && len < sizeof cl - 1; c++) cl[len++] = *c == '|' ? 0 : *c;
    if (len) cl[len++] = 0;
    snprintf(path, sizeof path, "%s/cmdline", dir);
    write_file(path, cl, len);
}

static void fake_stat(unsigned busy, unsigned idle)
{
    char s[128];
    snprintf(s, sizeof s, "cpu  %u 0 %u %u 20 0 0 0 0 0\ncpu0 1 1 1 1\n", busy * 2 / 3, busy - busy * 2 / 3, idle);
    write_file(FAKE_PROC "/stat", s, strlen(s));
}

/* A console-like system (the SD Debian). The pretend NZEUS is 120
 * (nzeus-modern); it was started by 115 (start-stop-daemon), started by 100
 * (the nzeus-deck start script), started by 1. */
static void make_fake_proc(void)
{
    if (system("rm -rf " FAKE_PROC) != 0) {}
    mkdir(FAKE_PROC, 0755);
    fake_proc(1, "init", 'S', 0, 380, "init");
    fake_proc(2, "kthreadd", 'S', 0, -1, "");
    fake_proc(3, "ksoftirqd/0", 'S', 2, -1, "");
    fake_proc(7, "kworker/0:1", 'I', 2, -1, "");
    fake_proc(95, "udevd", 'S', 1, 610, "/sbin/udevd|-d");
    fake_proc(100, "sh", 'S', 1, 520, "/bin/sh|/usr/local/bin/nzeus-deck|start");
    fake_proc(110, "dropbear", 'S', 1, 2900, "/usr/sbin/dropbear|-F|-s|-p|22");
    fake_proc(112, "sh", 'T', 1, 700, "/bin/sh|/usr/local/sbin/nzeus-usb-shell");
    fake_proc(115, "start-stop-daem", 'S', 100, 300, "start-stop-daemon|--start|--background");
    fake_proc(120, "nzeus-modern", 'R', 115, 14530, "/usr/local/bin/nzeus-modern");
    fake_proc(140, "busybox", 'S', 1, 1100, "busybox|udhcpc|-i|wlan0");
    fake_proc(150, "python3", 'S', 170, 8240, "/usr/bin/python3|-u|/tmp/nzeus-data/nzeus/python/my_robot.py");
    fake_proc(151, "httpd", 'S', 1, 420, "httpd|-h|/data/www");
    fake_proc(160, "wpa_supplicant", 'S', 1, 2100, "wpa_supplicant|-B|-i|wlan0");
    fake_proc(170, "sh", 'S', 120, 480, "sh|-c|python /tmp/nzeus-data/nzeus/python/my_robot.py");
    fake_proc(180, "sleep", 'Z', 170, -1, "");
    const char *mem = "MemTotal:          43008 kB\nMemFree:            6000 kB\nMemAvailable:      18000 kB\n"
                      "Buffers:            1000 kB\nSwapTotal:         65536 kB\nSwapFree:          60416 kB\n";
    write_file(FAKE_PROC "/meminfo", mem, strlen(mem));
    write_file(FAKE_PROC "/loadavg", "0.45 0.30 0.20 1/50 400\n", 24);
    write_file(FAKE_PROC "/uptime", "3725.50 3000.00\n", 16);
    fake_stat(150, 800);
}

static void protect_check(int pid, int want_refused)
{
    ProcInfo p;
    char what[160];
    int rc = procs_read(pid, &p);
    const char *why = rc == 0 ? procs_protected(&p, 120) : "(not found)";
    snprintf(what, sizeof what, "procs: %s %d (%s)  -> %s", want_refused ? "refuses to end" : "may end", pid, p.name, why ? why : "allowed");
    check(rc == 0 && (why != NULL) == want_refused, what);
}

static void test_procs(void)
{
    printf("--- procs ---\n");
    make_fake_proc();
    procs_set_root(FAKE_PROC);
    ProcInfo list[64], p;
    int n = procs_list(list, 64);
    check(n == 16, "procs: lists all 16 fake programs");
    check(n > 2 && list[0].pid == 120 && list[1].pid == 150 && list[2].pid == 110, "procs: biggest memory first (nzeus-modern, python3, dropbear)");
    int sorted = 1;
    for (int i = 1; i < n; i++) if (list[i].rss_kb > list[i - 1].rss_kb) sorted = 0;
    check(sorted, "procs: the whole list is sorted by memory");
    check(procs_read(120, &p) == 0 && !strcmp(p.name, "nzeus-modern") && p.ppid == 115 && p.state == 'R' && p.rss_kb == 14530 && !p.kernel,
          "procs: reads name, parent, state, memory of 120");
    check(procs_read(3, &p) == 0 && p.kernel, "procs: ksoftirqd is a kernel thread");
    check(procs_read(2, &p) == 0 && p.kernel, "procs: kthreadd is a kernel thread");
    check(procs_read(1, &p) == 0 && !p.kernel, "procs: init is not a kernel thread");
    check(procs_read(180, &p) == 0 && !p.kernel && p.state == 'Z', "procs: the zombie is a finished program, not a kernel thread");
    check(procs_read(999, &p) == -1, "procs: a missing PID gives -1");
    check(procs_alive(150) == 1 && procs_alive(180) == 0 && procs_alive(999) == 0, "procs: alive = running, a zombie counts as gone");
    check(!strcmp(procs_state_word('S'), "sleeping") && !strcmp(procs_state_word('T'), "paused"), "procs: state words");

    ProcSummary s;
    check(procs_summary(&s) == 0 && s.mem_total_kb == 43008 && s.mem_avail_kb == 18000 && s.swap_total_kb == 65536 &&
          s.swap_free_kb == 60416 && fabs(s.load1 - 0.45) < 1e-9 && fabs(s.uptime_s - 3725.5) < 1e-9, "procs: summary (meminfo, loadavg, uptime)");
    unsigned long long busy = 0, total = 0;
    check(procs_cpu_times(&busy, &total) == 0 && busy == 150 && total == 970, "procs: CPU busy/total from stat");

    protect_check(1, 1);       /* init */
    protect_check(2, 1);       /* kernel threads */
    protect_check(3, 1);
    protect_check(120, 1);     /* NZEUS itself */
    protect_check(115, 1);     /* started NZEUS (only the family rule knows this one) */
    protect_check(100, 1);     /* the nzeus-deck start script */
    protect_check(110, 1);     /* dropbear: the SSH server (the PC's way in) */
    protect_check(112, 1);     /* the USB serial rescue shell */
    protect_check(180, 1);     /* already finished */
    protect_check(150, 0);     /* a Python program: may be ended */
    protect_check(170, 0);     /* a shell running it (a child of NZEUS, not a parent) */
    protect_check(140, 0);     /* the DHCP client */
    protect_check(160, 0);
    fake_proc(400, "sh", 'S', 1, 500, "/bin/sh|/usr/local/bin/nzeus-deck|status");    /* a second nzeus-deck */
    protect_check(400, 1);
    if (system("rm -rf " FAKE_PROC "/400") != 0) {}

    errno = 0;
    check(procs_end(150, 0) == -1 && errno == EPERM, "procs: test build refuses to signal a PID the test didn't allow");
    check(procs_end(0, 1) == -1 && procs_end(-1, 1) == -1 && procs_end(1, 1) == -1, "procs: never signals 0, -1 or 1");

    /* the real /proc: this test and its parents are protected, a child is not */
    procs_set_root("/proc");
    check(procs_read(getpid(), &p) == 0 && procs_protected(&p, getpid()) != NULL, "procs (real /proc): this program is protected");
    check(procs_read(getppid(), &p) == 0 && procs_protected(&p, getpid()) != NULL, "procs (real /proc): its parent is protected");
    pid_t c = fork();
    if (c == 0) { for (;;) pause(); }
    check(procs_read(c, &p) == 0 && procs_protected(&p, getpid()) == NULL && !p.kernel, "procs (real /proc): a child may be ended");
    kill(c, SIGKILL);                                         /* (our own child) */
    waitpid(c, NULL, 0);
    procs_set_root(FAKE_PROC);
}

static void test_notes_names(void)
{
    printf("--- notes file names ---\n");
    struct tm tm = { 0 };
    tm.tm_year = 2026 - 1900; tm.tm_mon = 9; tm.tm_mday = 4; tm.tm_hour = 12;
    time_t when = mktime(&tm);
    char b[128];
    notes_safe_name("Shopping list!", when, b, sizeof b);
    check(!strcmp(b, "shopping-list-2026-10-04"), "notes: \"Shopping list!\" -> shopping-list-2026-10-04");
    notes_safe_name("../../etc/passwd", when, b, sizeof b);
    check(!strchr(b, '/') && !strchr(b, '.') && !strcmp(b, "etc-passwd-2026-10-04"), "notes: \"../../etc/passwd\" -> no slashes, no dots");
    notes_safe_name("   ", when, b, sizeof b);
    check(!strcmp(b, "note-2026-10-04"), "notes: an empty title -> note-2026-10-04");
    notes_safe_name("  Hello   World  ", when, b, sizeof b);
    check(!strcmp(b, "hello-world-2026-10-04"), "notes: spaces -> one dash, none at the ends");
    notes_safe_name("Caf\xC3\xA9 menu", when, b, sizeof b);
    check(!strcmp(b, "caf-menu-2026-10-04"), "notes: non-English letters are left out");
    notes_safe_name("a very very long title that goes on and on and on and on", when, b, sizeof b);
    check(strlen(b) <= 32 + 11, "notes: long titles are cut short");
    int ok = 1;
    for (const char *c = b; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) ok = 0;
    check(ok, "notes: only a-z, 0-9 and -");

    if (system("rm -rf /tmp/tools_names && mkdir -p /tmp/tools_names") != 0) {}
    char p1[256], p2[256], p3[256];
    notes_new_path("/tmp/tools_names", "Ideas", when, p1, sizeof p1);
    check(!strcmp(p1, "/tmp/tools_names/ideas-2026-10-04.txt"), "notes: new path is folder/name.txt");
    write_file(p1, "x", 1);
    notes_new_path("/tmp/tools_names", "Ideas", when, p2, sizeof p2);
    check(!strcmp(p2, "/tmp/tools_names/ideas-2026-10-04-2.txt"), "notes: same title again -> -2 (never overwrites)");
    write_file(p2, "x", 1);
    notes_new_path("/tmp/tools_names", "Ideas", when, p3, sizeof p3);
    check(!strcmp(p3, "/tmp/tools_names/ideas-2026-10-04-3.txt"), "notes: and -3");
}

/* ================= 2. the apps in NZEUS ================= */
static const char *shots = ".";

static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/tools_%s.ppm", shots, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6 %d %d 255\n", NZ_W, NZ_H);
    for (int i = 0; i < NZ_W * NZ_H; i++) {
        uint16_t p = nzeus_fb[i];
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) * 255 / 31), (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run(int ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)ms;
    do { ui_tick(); lv_timer_handler(); usleep(2000); } while ((int32_t)(end - nzeus_tick_ms()) > 0);
    lv_refr_now(NULL);
}

static void key(int k) { ui_key(k); run(5); }
static void kbd_key(KbKey k) { KbEvent e = { k, 0 }; ui_kbd(&e); run(5); }
static void type(const char *s)
{
    for (; *s; s++) {
        KbEvent e = { KB_CHAR, *s };
        if (*s == '\n') e.key = KB_ENTER;
        ui_kbd(&e);
    }
    run(5);
}

static int mx = NZ_W / 2, my = NZ_H / 2;
static void mouse_to(int x, int y) { mx = x; my = y; ui_mouse(mx, my, 0, 0, 1); run(30); }
static void mouse_click(void) { ui_mouse(mx, my, 1, 1, 1); run(40); ui_mouse(mx, my, 0, 0, 1); run(40); }
static void click_obj(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    mouse_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    mouse_click();
}

static int is_open(int id) { NzApp *a = apps_get(id); return a && a->root; }
static void close_app(int id) { NzApp *a = apps_get(id); if (a && a->root) apps_close(a); run(20); }
static void close_all(void) { for (int id = 0; id < APP_ALL_COUNT; id++) close_app(id); run(20); }

static void expect_expr(const char *want, const char *what)
{
    char m[200];
    snprintf(m, sizeof m, "calculator: %s -> \"%s\" (shows \"%s\")", what, want, app_calculator_expr());
    check(!strcmp(app_calculator_expr(), want), m);
}

static void test_calculator(void)
{
    printf("--- Calculator ---\n");
    apps_open(APP_CALCULATOR); run(50);
    check(!strcmp(app_calculator_focused(), "5"), "calculator: starts on the 5 key");
    key(NZ_KEY_DOWN); key(NZ_KEY_LEFT);                      /* 5 -> 2 -> 1 */
    check(!strcmp(app_calculator_focused(), "1"), "calculator: D-pad down, left -> the 1 key");
    key(NZ_KEY_A);
    for (int i = 0; i < 4; i++) key(NZ_KEY_RIGHT);           /* 1 -> 2 -> 3 -> . -> + */
    check(!strcmp(app_calculator_focused(), "+"), "calculator: right x4 -> the + key");
    key(NZ_KEY_A);
    for (int i = 0; i < 3; i++) key(NZ_KEY_LEFT);
    key(NZ_KEY_A);
    expect_expr("1+2", "D-pad typing 1 + 2");
    key(NZ_KEY_START);
    expect_expr("3", "START (=) on 1+2");
    check(strstr(app_calculator_top(), "1 + 2 =") != NULL, "calculator: the small line shows \"1 + 2 =\"");
    key(NZ_KEY_DOWN);                                        /* row 4: the wide 0 key, then = */
    check(!strcmp(app_calculator_focused(), "0"), "calculator: down -> the wide 0 key");
    key(NZ_KEY_RIGHT);
    check(!strcmp(app_calculator_focused(), "="), "calculator: right from 0 -> =");
    key(NZ_KEY_RIGHT);
    check(!strcmp(app_calculator_focused(), "="), "calculator: right at the edge stays on =");
    key(NZ_KEY_UP);
    check(!strcmp(app_calculator_focused(), "."), "calculator: up from = -> .");

    kbd_key(KB_ESC);
    expect_expr("", "Esc clears");
    type("2+3*4\n");   expect_expr("14", "keyboard 2+3*4 Enter");
    type("+1\n");      expect_expr("15", "an operator after an answer goes on from it");
    type("7");         expect_expr("7", "a digit after an answer starts a new sum");
    kbd_key(KB_ESC);
    type("(2+3)*4=");  expect_expr("20", "keyboard (2+3)*4=");
    kbd_key(KB_ESC);
    type("200+10%\n"); expect_expr("220", "200+10%");
    kbd_key(KB_ESC);
    type("7/0\n");
    check(!strcmp(app_calculator_top(), "Can't divide by zero"), "calculator: 7/0 says \"Can't divide by zero\"");
    expect_expr("7/0", "the sum stays after an error, so it can be fixed");
    kbd_key(KB_BACKSPACE); type("2\n");
    expect_expr("3.5", "fixing it: 7/2");
    kbd_key(KB_ESC);
    type("2+\n");
    check(!strcmp(app_calculator_top(), "Something is missing"), "calculator: 2+ says \"Something is missing\"");
    kbd_key(KB_ESC);
    type("1..5");      expect_expr("1.5", "a second dot in a number is ignored");
    kbd_key(KB_ESC);
    type("*");         expect_expr("0*", "an operator first starts from 0");
    type("-");         expect_expr("0-", "a new operator replaces the last one");
    kbd_key(KB_ESC);
    type("12");
    for (int i = 0; i < 4; i++) key(NZ_KEY_UP);              /* to the top row, then to +/- */
    for (int i = 0; i < 4; i++) key(NZ_KEY_LEFT);
    for (int i = 0; i < 3; i++) key(NZ_KEY_RIGHT);
    check(!strcmp(app_calculator_focused(), "+/-"), "calculator: found the +/- key");
    key(NZ_KEY_A);     expect_expr("-12", "+/- on 12");
    key(NZ_KEY_A);     expect_expr("12", "+/- again");
    type("+5");
    key(NZ_KEY_A);     expect_expr("12-5", "+/- on the 5 of 12+5");
    key(NZ_KEY_START); expect_expr("7", "12-5 =");
    check(app_calculator_history_count() == 5, "calculator: history keeps the last 5 answers");
    char longs[300];
    memset(longs, '9', 250); longs[250] = 0;
    kbd_key(KB_ESC);
    type(longs);
    check(strlen(app_calculator_expr()) == CALC_MAX_LEN, "calculator: typing stops at the length limit");
    kbd_key(KB_ESC);
    type("12");
    key(NZ_KEY_B);     expect_expr("1", "B deletes the last character");
    key(NZ_KEY_B);     expect_expr("", "B again");
    check(is_open(APP_CALCULATOR), "calculator: still open while B deleted");
    key(NZ_KEY_B);
    check(!is_open(APP_CALCULATOR), "calculator: B with nothing typed leaves");
    run(50);
}

/* the calculator in a desktop window, with the mouse */
static void test_calculator_mouse(void)
{
    ui_mouse(mx, my, 0, 0, 1); run(30);                      /* the mouse moved: desktop mode */
    check(ui_mode() == UI_DESKTOP, "desktop mode for the mouse test");
    apps_open(APP_CALCULATOR); run(80);
    click_obj(app_calculator_key_obj("7"));
    click_obj(app_calculator_key_obj("x"));
    click_obj(app_calculator_key_obj("6"));
    expect_expr("7*6", "mouse clicks 7 x 6 (in a window)");
    click_obj(app_calculator_key_obj("="));
    expect_expr("42", "mouse click =");
    type("1+1\n");
    expect_expr("2", "the keyboard types into the window in front");
    close_app(APP_CALCULATOR);
    ui_set_mode(UI_HANDHELD); run(20);
}

static int child_ready_pipe[2];
static pid_t start_child(int ignore_term)
{
    if (pipe(child_ready_pipe) != 0) return -1;
    pid_t c = fork();
    if (c == 0) {
        if (ignore_term) signal(SIGTERM, SIG_IGN);           /* a stubborn program */
        if (write(child_ready_pipe[1], "r", 1) != 1) _exit(1);
        for (;;) pause();
    }
    char b;
    if (read(child_ready_pipe[0], &b, 1) != 1) {}
    close(child_ready_pipe[0]);
    close(child_ready_pipe[1]);
    procs_test_allow(c);                                     /* the only PIDs the app may signal */
    return c;
}

/* the child's end: 0 = still running, else the signal that ended it */
static int child_end_signal(pid_t c)
{
    int st;
    if (waitpid(c, &st, WNOHANG) != c) return 0;
    return WIFSIGNALED(st) ? WTERMSIG(st) : -1;
}

static void test_taskmgr(void)
{
    printf("--- Task Manager ---\n");
    make_fake_proc();
    procs_set_root(FAKE_PROC);
    procs_test_set_self(120);                                /* pretend NZEUS is nzeus-modern 120 */
    apps_open(APP_TASKMGR); run(50);
    check(app_taskmgr_count() == 12, "taskmgr: 12 programs (kernel threads and the finished one hidden)");
    check(app_taskmgr_selected_pid() == 120, "taskmgr: the biggest (nzeus-modern) is selected first");
    key(NZ_KEY_A);
    check(!nzui_active(), "taskmgr: A on nzeus-modern does not ask (it is protected: a message says why)");
    app_taskmgr_select_pid(115);
    key(NZ_KEY_X);
    check(!nzui_active(), "taskmgr: X on taskset (started NZEUS) is refused too");
    app_taskmgr_select_pid(150);
    key(NZ_KEY_A);
    check(nzui_active(), "taskmgr: A on python asks \"End python?\"");
    key(NZ_KEY_A);                                           /* yes: the test build refuses the fake PID */
    check(!nzui_active() && access(FAKE_PROC "/150", F_OK) == 0, "taskmgr: the fake python is untouched (only test children can be signalled)");

    app_taskmgr_select_pid(140);                             /* the selection survives a refresh... */
    fake_proc(140, "dropbear", 'S', 1, 30000, "dropbear|-R");   /* ...even when it moves to the top */
    fake_proc(190, "newprog", 'R', 1, 900, "newprog");
    app_taskmgr_refresh();
    check(app_taskmgr_count() == 13 && app_taskmgr_selected_pid() == 140, "taskmgr: refresh keeps dropbear selected (now first, list grew)");
    if (system("rm -rf " FAKE_PROC "/190") != 0) {}
    app_taskmgr_refresh();
    check(app_taskmgr_count() == 12 && app_taskmgr_selected_pid() == 140, "taskmgr: a program that ended disappears, selection kept");
    close_app(APP_TASKMGR);

    /* real programs: two children of this test, nothing else */
    procs_set_root("/proc");
    procs_test_set_self(0);
    pid_t polite = start_child(0), stubborn = start_child(1);
    apps_open(APP_TASKMGR); run(50);
    if (app_taskmgr_select_pid(polite) >= 0) {
        key(NZ_KEY_A);
        check(nzui_active(), "taskmgr (real): A on our child asks first");
        key(NZ_KEY_A);
        run(500);
        check(child_end_signal(polite) == SIGTERM, "taskmgr (real): the child ended with SIGTERM");
    } else check(0, "taskmgr (real): our child is in the list");
    run(400);                                                /* the app notices it has gone */
    check(app_taskmgr_select_pid(polite) < 0, "taskmgr (real): the ended child left the list");
    if (app_taskmgr_select_pid(stubborn) >= 0) {
        key(NZ_KEY_A); key(NZ_KEY_A);                        /* End? yes */
        run(800);
        check(!nzui_active() && child_end_signal(stubborn) == 0, "taskmgr (real): the stubborn child ignores SIGTERM; no Force stop yet");
        run(1700);
        check(nzui_active(), "taskmgr (real): after 2 s it offers Force stop");
        key(NZ_KEY_A);
        run(300);
        check(child_end_signal(stubborn) == SIGKILL, "taskmgr (real): Force stop ended it with SIGKILL");
    } else check(0, "taskmgr (real): the stubborn child is in the list");
    kill(polite, SIGKILL); kill(stubborn, SIGKILL);          /* (our own children, in case a check failed) */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    close_app(APP_TASKMGR);
    procs_set_root(FAKE_PROC);
    procs_test_set_self(120);
}

static void write_note(const char *name, const char *text, int minutes_ago)
{
    char p[256];
    snprintf(p, sizeof p, NOTES "/%s", name);
    write_file(p, text, strlen(text));
    struct timespec t[2];
    t[0].tv_sec = t[1].tv_sec = time(NULL) - minutes_ago * 60;
    t[0].tv_nsec = t[1].tv_nsec = 0;
    utimensat(AT_FDCWD, p, t, 0);
}

static void test_notes(void)
{
    printf("--- Notes ---\n");
    if (system("rm -rf " NOTES) != 0) {}
    apps_open(APP_NOTES); run(50);
    struct stat st;
    check(stat(NOTES, &st) == 0 && S_ISDIR(st.st_mode), "notes: makes the nzeus/notes folder");
    check(app_notes_count() == 0, "notes: empty at first");
    key(NZ_KEY_X);
    check(nzui_active(), "notes: X asks for a title");
    type("Shopping list\n");
    run(50);
    char want[256];
    snprintf(want, sizeof want, NOTES "/");
    notes_safe_name("Shopping list", time(NULL), want + strlen(want), sizeof want - strlen(want));
    strcat(want, ".txt");
    check(!strcmp(app_editor_path(), want), "notes: the new note opens in the Editor");
    check(is_open(APP_EDITOR) && apps_focused() == apps_get(APP_EDITOR), "notes: the Editor is in front");
    FILE *f = fopen(want, "r");
    char line[128] = "";
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    check(!strcmp(line, "Shopping list\n"), "notes: the first line is the title");
    f = fopen(want, "a");                                    /* "typed in the Editor and saved" */
    if (f) { fputs("eggs, milk, solder\n", f); fclose(f); }
    close_app(APP_EDITOR);
    check(apps_focused() == apps_get(APP_NOTES), "notes: closing the Editor goes back to Notes");
    run(1200);
    check(app_notes_count() == 1 && !strcmp(app_notes_title(0), "Shopping list"), "notes: the list shows the new note");

    key(NZ_KEY_X); type("Shopping list\n"); run(50);         /* the same title again: a new file */
    check(strstr(app_editor_path(), "-2.txt") != NULL, "notes: same title -> a second file (-2)");
    close_app(APP_EDITOR);
    run(1200);
    check(app_notes_count() == 2, "notes: two notes");

    write_note("from-pc.txt", "Made outside\nby another program", 0);   /* a file appears by itself */
    run(1300);
    check(app_notes_count() == 3, "notes: a new file on the card shows up within a second");

    key(NZ_KEY_UP); key(NZ_KEY_UP); key(NZ_KEY_UP);            /* row 0: "New note" */
    key(NZ_KEY_Y);
    check(!nzui_active(), "notes: Y on \"New note\" deletes nothing");
    key(NZ_KEY_DOWN);                                        /* the newest note */
    char victim[256];
    snprintf(victim, sizeof victim, "%s", app_notes_path(0));
    key(NZ_KEY_A);
    check(!strcmp(app_editor_path(), victim), "notes: A opens the selected note in the Editor");
    close_app(APP_EDITOR);
    run(50);
    key(NZ_KEY_Y);
    check(nzui_active(), "notes: Y asks \"Delete ...?\"");
    key(NZ_KEY_B);
    check(access(victim, F_OK) == 0, "notes: B (no) keeps the note");
    key(NZ_KEY_Y); key(NZ_KEY_A);
    check(access(victim, F_OK) != 0 && app_notes_count() == 2, "notes: A (yes) deletes it");
    close_all();
}

static void test_clock(void)
{
    printf("--- Clock ---\n");
    apps_open(APP_CLOCK); run(50);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char hm[8], year[16];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    snprintf(year, sizeof year, "%d", tm.tm_year + 1900);
    check(app_clock_page() == 0 && !strncmp(app_clock_text(0), hm, 5) && strlen(app_clock_text(0)) == 8, "clock: shows the time as HH:MM:SS");
    check(strstr(app_clock_text(1), year) != NULL, "clock: the date in words has the year");
    char before[16];
    snprintf(before, sizeof before, "%s", app_clock_text(0));
    run(1100);
    check(strcmp(before, app_clock_text(0)) != 0, "clock: the seconds move");

    key(NZ_KEY_R);
    check(app_clock_page() == 1, "clock: R1 -> Calendar");
    static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char m[32];
    snprintf(m, sizeof m, "%s %d", months[tm.tm_mon], tm.tm_year + 1900);
    check(!strcmp(app_clock_text(4), m), "clock: the calendar starts on this month");
    for (int i = 0; i < 12; i++) key(NZ_KEY_RIGHT);
    snprintf(m, sizeof m, "%s %d", months[tm.tm_mon], tm.tm_year + 1901);
    check(!strcmp(app_clock_text(4), m), "clock: RIGHT x12 -> the same month next year");
    key(NZ_KEY_UP);
    snprintf(m, sizeof m, "%s %d", months[tm.tm_mon], tm.tm_year + 1900);
    check(!strcmp(app_clock_text(4), m), "clock: UP -> a year back");
    key(NZ_KEY_LEFT); key(NZ_KEY_A);
    snprintf(m, sizeof m, "%s %d", months[tm.tm_mon], tm.tm_year + 1900);
    check(!strcmp(app_clock_text(4), m), "clock: A -> back to today");

    key(NZ_KEY_R);
    check(app_clock_page() == 2 && !strcmp(app_clock_text(2), "00:00.00"), "clock: Stopwatch at 00:00.00");
    key(NZ_KEY_START); run(300);
    check(strcmp(app_clock_text(2), "00:00.00") != 0, "clock: START runs the stopwatch");
    key(NZ_KEY_X); run(100); key(NZ_KEY_X);
    check(app_clock_lap_count() == 2, "clock: X takes laps");
    key(NZ_KEY_START);
    snprintf(before, sizeof before, "%s", app_clock_text(2));
    run(300);
    check(!strcmp(before, app_clock_text(2)), "clock: START again stops it");
    key(NZ_KEY_Y);
    check(!strcmp(app_clock_text(2), "00:00.00") && app_clock_lap_count() == 0, "clock: Y resets");

    key(NZ_KEY_R);
    check(app_clock_page() == 3 && !strcmp(app_clock_text(3), "05:00"), "clock: Timer starts at 05:00");
    key(NZ_KEY_UP);
    check(!strcmp(app_clock_text(3), "06:00"), "clock: UP adds a minute");
    key(NZ_KEY_DOWN); key(NZ_KEY_DOWN); key(NZ_KEY_DOWN); key(NZ_KEY_DOWN); key(NZ_KEY_DOWN); key(NZ_KEY_DOWN);
    check(!strcmp(app_clock_text(3), "01:00"), "clock: DOWN stops at 1 minute");
    key(NZ_KEY_START); run(1100);
    check(!strcmp(app_clock_text(3), "00:59"), "clock: START counts down");
    app_clock_timer_skip(58500);                             /* fast-forward to the end */
    run(800);
    check(app_clock_overlay_shown(), "clock: at zero, \"Time's up!\" covers the app");
    key(NZ_KEY_B);
    check(!app_clock_overlay_shown() && is_open(APP_CLOCK), "clock: a button closes the message (B doesn't leave)");
    key(NZ_KEY_R);
    check(app_clock_page() == 0, "clock: R1 on the last page wraps to Clock");
    key(NZ_KEY_L);
    check(app_clock_page() == 3, "clock: L1 wraps back");
    KbEvent tab = { KB_TAB, 0 };
    ui_kbd(&tab); run(5);
    check(app_clock_page() == 0, "clock: the keyboard's Tab switches pages too");
    close_all();
}

/* ================= 3. memory ================= */
static void write_text(const char *path, const char *text) { write_file(path, text, strlen(text)); }

/* ---- Wi-Fi ID cards (wifiinfo.c), on the console's REAL scans ----
 * (the layout of the console's real scans; names and addresses made up) */
static const char *const real_survey =
    "index  bssid               ch  RSSI  SdBm  Noise    age                              flag                              ssid\n"
    "    1  82:11:22:33:44:90    3  -64   -66     0     7659                  [WPA2][WEP][ESS]                        HomeNet\n"
    "    2  00:1b:2c:00:00:24   10  -86   -86     0     6981             [WPA2][WEP][WPS][ESS]               Camera_1234\n"
    "    3  82:11:22:33:44:94   36  -70   -71     0     6536                  [WPA2][WEP][ESS]                        HomeNet\n"
    "    4  82:11:22:33:44:98   36  -70   -71     0     6516                  [WPA2][WEP][ESS]                     HomeNet_5G\n"
    "    5  48:55:66:77:88:33   11  -86   -88     0     6794             [WPA2][WEP][WPS][ESS]                            ISP 5G\n"
    "    6  4a:55:66:77:88:37   11  -90   -90     0     6792                  [WPA2][WEP][ESS]                                  \n";

static void test_wifiinfo(void)
{
    printf("--- wifiinfo (real scans) ---\n");
    WiNet w[16];
    char s[512], why[128];
    int n = wi_parse(real_survey, w, 16);
    check(n == 6, "wifi: all 6 rows of the real scan are read (the same name twice is kept)");
    check(!strcmp(w[0].ssid, "HomeNet") && w[0].channel == 3 && w[0].dbm == -66 && !strcmp(w[0].flags, "[WPA2][WEP][ESS]"),
          "wifi: name, channel, signal and flags of a real row");
    check(!strcmp(w[4].ssid, "ISP 5G"), "wifi: a name with a space (ISP 5G)");
    check(w[5].ssid[0] == 0 && w[5].channel == 11, "wifi: a hidden network (only spaces where the name is)");
    check(wi_security(w[0].flags, s, sizeof s) == 3 && strstr(s, "WPA2"),
          "wifi: [WPA2][WEP] is WPA2 (this driver adds [WEP] to WPA2 networks) - not \"unsafe\"");
    check(wi_security("[WEP][ESS]", s, sizeof s) == 1 && strstr(s, "unsafe"), "wifi: WEP alone is unsafe");
    check(wi_security("[ESS]", s, sizeof s) == 0 && strstr(s, "Open"), "wifi: no flags = an open network");
    check(wi_security("[WPA][ESS]", s, sizeof s) == 2, "wifi: old WPA");
    check(wi_security("[WPA3-SAE][ESS]", s, sizeof s) == 4, "wifi: WPA3");
    check(wi_wps(w[1].flags) && !wi_wps(w[0].flags), "wifi: WPS on/off");
    check(!strcmp(wi_band(3), "2.4 GHz") && !strcmp(wi_band(36), "5 GHz") && !strcmp(wi_band(0), "?"), "wifi: bands");
    check(wi_private_mac("82:11:22:33:44:90") && !wi_private_mac("00:1b:2c:00:00:24"),
          "wifi: 82:... is a private address (no maker in it), 00:1b:2c is not");
    check(wi_same_router(w[0].bssid, w[2].bssid), "wifi: HomeNet on channel 3 and 36 = the same router (two bands)");
    check(wi_same_router(w[4].bssid, w[5].bssid), "wifi: ISP 5G and the hidden one next to it = the same router");
    check(!wi_same_router(w[0].bssid, w[1].bssid), "wifi: the Arlo camera is a different device");
    check(!wi_same_router("aa:bb:cc:11:22:33", "aa:bb:cc:44:55:66"), "wifi: two routers of the same maker are different");
    check(wi_twins(w, n, 0) == 0, "wifi: my own two-band router is NOT reported as a fake twin");
    WiNet fake[2] = { w[0], w[0] };
    snprintf(fake[1].bssid, sizeof fake[1].bssid, "00:11:22:33:44:55");
    check(wi_twins(fake, 2, 0) == 1, "wifi: the same name from a different router IS reported");
    wi_signal(-66, 3, s, sizeof s);
    check(!strcmp(s, "-66 dBm, good (3 of 4 bars), about 7 m away"), "wifi: signal words + a rough distance (-66 dBm)");
    wi_signal(-86, 10, s, sizeof s);
    check(strstr(s, "weak (1 of 4 bars), about 34 m") != NULL, "wifi: a weak signal is far away");
    check(wi_best_channel(w, n, "HomeNet", why, sizeof why) == 1 && !strcmp(why, "Channel 1: 0 networks, 6: 1, 11: 3 nearby"),
          "wifi: best channel for your router (ignoring it) = 1, the quiet one");
    /* counting it too: 1 has the router (-66: 34), 11 has three weak ones (14+12+10 = 36) -> still 1 */
    check(wi_best_channel(w, n, NULL, why, sizeof why) == 1 && !strcmp(why, "Channel 1: 1 network, 6: 2, 11: 3 nearby"),
          "wifi: one strong network weighs more than three weak ones");
    {   /* one loud neighbour on 1 and one on 6, two faint ones on 11: 11 is best (counting alone would say 1) */
        WiNet m[4];
        memset(m, 0, sizeof m);
        m[0].channel = 1;  m[0].dbm = -40;
        m[1].channel = 6;  m[1].dbm = -45;
        m[2].channel = 11; m[2].dbm = -90;
        m[3].channel = 11; m[3].dbm = -90;
        check(wi_best_channel(m, 4, NULL, why, sizeof why) == 11, "wifi: two faint networks are better neighbours than one loud one");
    }
    write_text("/tmp/tools_oui.txt", "00-1B-2C   (hex)\t\tTest Maker Ltd\n001A2B Other Maker\n");
    check(wi_maker(w[1].bssid, "/tmp/tools_oui.txt", s, sizeof s) == 0 && !strcmp(s, "Test Maker Ltd"),
          "wifi: the maker from the IEEE list's own layout (CC-40-D0 (hex) ...)");
    check(wi_maker("00:1a:2b:00:00:01", "/tmp/tools_oui.txt", s, sizeof s) == 0 && !strcmp(s, "Other Maker"),
          "wifi: the maker from a short AABBCC line");
    check(wi_maker(w[0].bssid, "/tmp/tools_oui.txt", s, sizeof s) == -1 && strstr(s, "private address"),
          "wifi: a private address has no maker (says so, no guess)");
    check(wi_maker(w[1].bssid, "/tmp/tools_no_such_list", s, sizeof s) == -2 && strstr(s, "no maker list"),
          "wifi: no list on the card -> says so");
    char card[1024];
    wi_card(w, n, 0, "/tmp/tools_no_such_list", card, sizeof card);
    check(strstr(card, "Band      2.4 GHz, channel 3") && strstr(card, "Security  WPA2 - good") && strstr(card, "WPS       off") &&
          strstr(card, "Same router  HomeNet (ch 36)") && !strstr(card, "Check"),
          "wifi: the ID card of my router");
    wi_card(w, n, 5, NULL, card, sizeof card);
    check(strstr(card, "(hidden - it doesn't send its name)") && strstr(card, "Same router  ISP 5G (ch 11)"),
          "wifi: the hidden network's card names its router's other network");
}

/* ---- Bluetooth ID cards (btinfo.c), from `nzeus-bt survey` lines ---- */
static void bt_has(const char *line, const char *want, const char *what)
{
    BtSurvey d;
    char card[1200], msg[300];
    int rc = bt_parse_survey(line, &d);
    bt_card(&d, "/tmp/tools_oui.txt", card, sizeof card);
    snprintf(msg, sizeof msg, "bt: %s  (wanted \"%.70s\")", what, want);
    check(rc == 0 && strstr(card, want) != NULL, msg);
    if (rc != 0 || !strstr(card, want)) printf("      card was: %s\n", card);
}

static void test_btinfo(void)
{
    printf("--- btinfo ---\n");
    BtSurvey d;
    /* (the same line linux-port/deck-modern/bt/tests/test_flows.py expects from its fake LE mouse) */
    const char *mouse = "C4:00:00:00:00:05\tle-random\t-55\t-8\t-\t03c2\t1812\t01da:0102\tLE Mouse";
    check(bt_parse_survey(mouse, &d) == 0 && !strcmp(d.addr, "C4:00:00:00:00:05") && !strcmp(d.type, "le-random") &&
          d.rssi == -55 && d.has_tx && d.tx_power == -8 && !d.has_cod && d.has_app && d.appearance == 0x03c2 &&
          d.n_uuid == 1 && d.uuid[0] == 0x1812 && d.has_mfr && d.company == 0x01da && d.mfr_len == 2 && d.mfr[1] == 0x02 &&
          !strcmp(d.name, "LE Mouse"), "bt: a survey line is read field by field");
    check(bt_parse_survey("not a survey line", &d) == -1, "bt: other text is refused");
    write_text("/tmp/tools_oui.txt", "00-1B-2C   (hex)\t\tTest Maker Ltd\n001A2B Other Maker\n");
    bt_has(mouse, "Likely: a mouse - from the kind it announces (medium)", "LE mouse: kind from its appearance");
    bt_has(mouse, "Maker     Logitech (from its maker code)", "LE mouse: maker from its company number");
    bt_has(mouse, "made up by the device, but it keeps it", "C4:... = a random but fixed address");
    bt_has(mouse, "very close (under 1 m)  (sends at -8 dBm)", "signal + its transmit power");
    bt_has(mouse, "Raw data  01DA:0102", "the raw maker bytes are shown for tinkering");
    bt_has("7A:11:22:33:44:55\tle-random\t-50\t-\t-\t-\t-\t004c:0719010e20\t(no name)",
           "Likely: Apple AirPods Pro - from Apple's model code in its advert (high)", "AirPods Pro from Apple's model code");
    bt_has("7A:11:22:33:44:55\tle-random\t-50\t-\t-\t-\t-\t004c:0719010e20\t(no name)",
           "private, changes every few minutes", "7A:... = a private address that changes");
    bt_has("7A:11:22:33:44:56\tle-random\t-50\t-\t-\t-\t-\t004c:0719019920\t(no name)",
           "model code 9920, not in our list", "an unknown Apple model says so (no made-up name)");
    bt_has("4F:11:22:33:44:55\tle-random\t-70\t-\t-\t-\t-\t004c:1219100000000000\t(no name)",
           "Tracker   yes", "a Find My advert = a tracker");
    bt_has("4F:11:22:33:44:55\tle-random\t-70\t-\t-\t-\t-\t004c:1219100000000000\t(no name)",
           "AirTag or other Find My item", "Find My: the guess");
    bt_has("D1:22:33:44:55:66\tle-random\t-80\t-\t-\t-\tfeed\t-\tTile",
           "Likely: a Tile tracker - from its Tile service (high)", "Tile from its service");
    bt_has("D1:22:33:44:55:66\tle-random\t-80\t-\t-\t-\tfd5a\t-\t(no name)", "Samsung SmartTag", "SmartTag from its service");
    bt_has("D1:22:33:44:55:67\tle-random\t-60\t-\t-\t-\tfe2c,180f\t-\tBuds", "Google Fast Pair", "Fast Pair from its service");
    bt_has("D1:22:33:44:55:67\tle-random\t-60\t-\t-\t-\tfe2c,180f\t-\tBuds", "Services  Google Fast Pair, battery", "services in words");
    bt_has("D1:22:33:44:55:68\tle-random\t-60\t-\t-\t-\t-\t0006:030080\t(no name)", "Microsoft Swift Pair", "Swift Pair");
    bt_has("D1:22:33:44:55:69\tle-random\t-60\t-\t-\t-\t-\t004c:0215" "00112233445566778899aabbccddeeff" "0001000209c5\t(no name)",
           "An iBeacon", "an iBeacon");
    bt_has("AC:37:43:00:00:04\tclassic\t-50\t-\t5a020c\t-\t-\t-\tPhone", "Likely: a phone - from its device class (medium)", "classic phone from its class");
    bt_has("00:1A:2B:00:00:09\tclassic\t-75\t-\t-\t-\t-\t-\t(no name)", "Maker     Other Maker (from its address)",
           "classic: maker from the IEEE list on the card");
    bt_has("D2:00:00:00:00:01\tle-random\t-90\t-\t-\t-\t-\t1234:00\t(no name)", "company #1234 (not in the built-in list)",
           "an unknown company shows its number, no guess");
    bt_has("D2:00:00:00:00:02\tle-random\t-90\t-\t-\t-\t-\t-\t(no name)", "Not enough clues - it only sends its address (low)",
           "no clues at all: says so");
    bt_has("D2:00:00:00:00:02\tle-random\t-90\t-\t-\t-\t-\t-\t(no name)", "about 17 m away", "a weak signal: further away");
    check(bt_parse_survey("D1:22:33:44:55:66\tle-random\t-80\t-\t-\t-\tfeed\t-\tTile", &d) == 0 && bt_is_tracker(&d) &&
          bt_parse_survey(mouse, &d) == 0 && !bt_is_tracker(&d), "bt: trackers yes, a mouse no");
}

/* ---- the Health dashboard's readings and verdict (health.c) ---- */
static HealthInfo healthy(void)                     /* a console with nothing wrong */
{
    HealthInfo h;
    memset(&h, 0, sizeof h);
    h.mem_total_kb = 43160; h.mem_avail_kb = 20000;
    h.battery = 80; h.sd_total_mb = 16000; h.sd_free_mb = 13000;
    h.temp_c = HEALTH_NO_TEMP;  h.load1 = 0.4;
    return h;
}
static void verdict_has(HealthInfo h, int worries, const char *want, const char *what)
{
    char out[512], msg[300];
    int n = health_verdict(&h, out, sizeof out);
    snprintf(msg, sizeof msg, "health: %s  (got %d: \"%.120s\")", what, n, out);
    check(n == worries && strstr(out, want) != NULL, msg);
}

static void test_health(void)
{
    printf("--- health ---\n");
    HealthInfo h = healthy();
    verdict_has(h, 0, "All good", "nothing wrong -> \"All good\"");
    h = healthy(); h.mem_avail_kb = 2 * 1024;
    verdict_has(h, 1, "Memory is very low (2 MB free)", "under 3 MB free: very low");
    h = healthy(); h.mem_avail_kb = 5 * 1024;
    verdict_has(h, 1, "Memory is getting low (5 MB free)", "under 6 MB free: getting low");
    h = healthy(); h.mem_avail_kb = 7 * 1024;
    verdict_has(h, 0, "All good", "7 MB free is fine");
    h = healthy(); h.battery = 8;
    verdict_has(h, 1, "Battery almost empty (8%)", "battery 8%: almost empty");
    h = healthy(); h.battery = 8; h.charging = 1;
    verdict_has(h, 0, "All good", "battery 8% but charging: no worry");
    h = healthy(); h.battery = 18;
    verdict_has(h, 1, "Battery low (18%)", "battery 18%: low");
    h = healthy(); h.battery = -1;
    verdict_has(h, 0, "All good", "battery unknown: not a worry");
    h = healthy(); h.sd_free_mb = 50;
    verdict_has(h, 1, "SD card almost full (50 MB free)", "50 MB left on the card");
    h = healthy(); h.swap_total_kb = 24 * 1024; h.swap_used_kb = 22 * 1024;
    verdict_has(h, 1, "zram is nearly full (22 of 24 MB)", "zram over 85% used");
    h = healthy(); h.temp_c = 90;
    verdict_has(h, 1, "Running hot (90 C)", "90 C: hot");
    h = healthy(); h.temp_c = 60;
    verdict_has(h, 0, "All good", "60 C is fine");
    h = healthy(); h.load1 = 5;
    verdict_has(h, 1, "The processor is very busy (load 5.0)", "load 5: busy");
    h = healthy(); h.battery = 5; h.mem_avail_kb = 1024;
    verdict_has(h, 2, "Memory is very low (1 MB free) - close an app\nBattery almost empty (5%)", "two worries, one per line, memory first");

    char d[32];
    health_duration(45, d, sizeof d);    check(!strcmp(d, "45 s"), "health: 45 s");
    health_duration(3725, d, sizeof d);  check(!strcmp(d, "1 h 2 min"), "health: 3725 s = 1 h 2 min");
    health_duration(90000, d, sizeof d); check(!strcmp(d, "1 day 1 h"), "health: 90000 s = 1 day 1 h");

    /* reading it all, from fake files */
    char old_root[256];
    snprintf(old_root, sizeof old_root, "%s", procs_root());   /* (a copy: procs_root() is procs.c's own buffer) */
    make_fake_proc();
    procs_set_root(FAKE_PROC);
    write_text("/tmp/tools_temp", "47000\n");
    health_thermal_path = "/tmp/tools_temp";
    health_sd_path = "/tmp";
    HealthInfo r;
    health_read(&r);
    check(r.mem_total_kb > 0 && r.temp_c == 47 && r.sd_total_mb > 0 && r.ntop > 0 && r.top_kb[0] >= r.top_kb[r.ntop - 1], "health: reads live memory, temperature (47000 = 47 C), the card and the biggest programs");
    unlink("/tmp/tools_temp");
    health_read(&r);
    check(r.temp_c == HEALTH_NO_TEMP, "health: no temperature sensor -> \"not measured\", never a made-up number");
    procs_set_root(old_root);
}

/* the Dev toolbox on screen: tools, encode <-> decode, copy and paste */
static void test_devtools_app(void)
{
    printf("--- Dev toolbox app ---\n");
    close_all();
    apps_open(APP_DEVTOOLS); run(30);
    check(is_open(APP_DEVTOOLS) && app_devtools_tool() == 0 && strstr(app_devtools_out(), "Decimal   255"),
          "devtools: opens on Number bases, already showing its example (0xFF = 255)");
    key(NZ_KEY_UP);
    check(app_devtools_tool() == 8, "devtools: UP from the first tool wraps to the last (Regex)");
    key(NZ_KEY_DOWN); key(NZ_KEY_DOWN);
    check(app_devtools_tool() == 1 && !strcmp(app_devtools_out(), "SGVsbG8gTlpFVVM="), "devtools: Base64 encodes its example");
    key(NZ_KEY_X);
    check(!strcmp(app_devtools_in(), "SGVsbG8gTlpFVVM=") && !strcmp(app_devtools_out(), "Hello NZEUS"),
          "devtools: X turns it around - the answer becomes the input and is decoded");
    key(NZ_KEY_Y);
    check(!strcmp(nz_clip_get(), "Hello NZEUS"), "devtools: Y copies the answer to the clipboard");
    for (int i = 0; i < 5; i++) key(NZ_KEY_DOWN);
    nz_clip_set("0, 255, 0");
    key(NZ_KEY_L);
    check(app_devtools_tool() == 6 && !strcmp(app_devtools_in(), "0, 255, 0") && strstr(app_devtools_out(), "Hex      #00FF00"),
          "devtools: L1 pastes the clipboard as the input (Colour)");
    key(NZ_KEY_DOWN);
    nz_clip_set("{\"broken\":");
    key(NZ_KEY_L);
    check(strstr(app_devtools_out(), "Not valid JSON") != NULL, "devtools: JSON says what is wrong with bad JSON");
    key(NZ_KEY_A);
    check(nzui_active(), "devtools: A opens the keyboard to type the input");
    key(NZ_KEY_B);
    check(!nzui_active() && !strcmp(app_devtools_in(), "{\"broken\":"), "devtools: B cancels the typing, the input stays");
    key(NZ_KEY_B);
    check(!is_open(APP_DEVTOOLS), "devtools: B leaves");
}

/* the Health dashboard on screen (the fake files from test_health) */
static void test_health_app(void)
{
    printf("--- Health app ---\n");
    close_all();
    write_text("/tmp/tools_temp", "91000\n");
    apps_open(APP_HEALTH); run(30);
    check(is_open(APP_HEALTH) && strstr(app_health_verdict(), "Running hot (91 C)"), "Health app: the worry is on top (running hot)");
    write_text("/tmp/tools_temp", "47000\n");
    check(strstr(app_health_value(6), "MB") != NULL && !strstr(app_health_value(6), "ran"),
          "Health app: live biggest processes follow uptime, without an unsupported last-start row");
    check(strstr(app_health_value(0), "MB free of") != NULL && strstr(app_health_value(3), "free of") != NULL,
          "Health app: memory and SD card");
    run(2100);                                               /* it refreshes by itself every 2 s */
    check(!strstr(app_health_verdict(), "Running hot") && !strcmp(app_health_value(4), "47 C"),
          "Health app: refreshes by itself (cooler: the worry went away, the temperature shown)");
    key(NZ_KEY_B);
    check(!is_open(APP_HEALTH), "Health app: B leaves");
}

static size_t heap_used(void) { run(60); return mallinfo2().uordblks; }

static void test_leaks(int id, const char *name, int window)
{
    char what[160];
    if (window) { ui_set_mode(UI_DESKTOP); run(20); }
    for (int i = 0; i < 3; i++) { apps_open(id); run(10); close_app(id); }     /* warm up (first-time allocations) */
    size_t before = heap_used();
    for (int i = 0; i < 50; i++) {
        apps_open(id);
        run(10);
        if (id == APP_CALCULATOR) type("12+34*5\n");          /* use it a little */
        if (id == APP_CLOCK) { ui_key(NZ_KEY_R); ui_key(NZ_KEY_R); ui_key(NZ_KEY_START); ui_key(NZ_KEY_X); }
        close_app(id);
        if (getenv("LEAK_TRACE") && i % 10 == 9) printf("  %s cycle %d: heap %+ld\n", name, i + 1, (long)heap_used() - (long)before);
    }
    size_t after = heap_used();
    long grew = (long)after - (long)before;
    snprintf(what, sizeof what, "memory: %s opened and closed 50 times%s: heap %+ld bytes (allowed: under 2048)",
             name, window ? " in a window" : "", grew);
    check(grew < 2048, what);
    if (window) { ui_set_mode(UI_HANDHELD); run(20); }
}

/* ================= 4. pictures ================= */
static void prepare_notes_for_pictures(void)
{
    if (system("rm -rf " NOTES) != 0) {}
    mkdir(NOTES, 0755);
    write_note("shopping-list.txt", "Shopping list\n\nresistors 220R, 10k\nLEDs (green)\nsolder wick\n", 12);
    write_note("launch-checklist.txt", "Launch checklist\n\n1. Charge the battery\n2. Check the parachute\n", 60 * 26);
    write_note("project-ideas.txt", "Project ideas\nA weather station; a new case design\n", 60 * 24 * 3);
    write_note("wifi.txt", "Wi-Fi dongle notes\nThe LB-Link shows up as a CD drive first.\n", 60 * 24 * 40);
    write_note("todo.md", "# Today\n\n- finish the Clock app\n- test on the console\n", 3);
}

static void pictures(int th, const char *tname)
{
    char n[64];
    theme_set(th);
    ui_set_mode(UI_HANDHELD);
    close_all();
    run(1900);                                               /* let messages disappear */

    make_fake_proc();
    apps_open(APP_TASKMGR); run(50);
    fake_stat(150 + 370, 800 + 630);                          /* 37 % busy since the first reading */
    app_taskmgr_select_pid(150);
    app_taskmgr_refresh(); run(50);
    snprintf(n, sizeof n, "%s_taskmgr", tname); save(n);
    key(NZ_KEY_A); run(50);
    snprintf(n, sizeof n, "%s_taskmgr_confirm", tname); save(n);
    key(NZ_KEY_B);
    close_all();

    apps_open(APP_CALCULATOR); run(50);
    type("1250*12\n"); type("-15%\n"); type("1/3\n"); type("(4+8)*2.5\n");
    type("355/113-3.14");
    run(50);
    snprintf(n, sizeof n, "%s_calculator", tname); save(n);
    type("/0\n"); run(30);
    snprintf(n, sizeof n, "%s_calculator_error", tname); save(n);
    close_all();

    prepare_notes_for_pictures();
    apps_open(APP_NOTES); run(50);
    snprintf(n, sizeof n, "%s_notes", tname); save(n);
    close_all();

    apps_open(APP_CLOCK); run(50);
    snprintf(n, sizeof n, "%s_clock", tname); save(n);
    key(NZ_KEY_R); run(30);
    snprintf(n, sizeof n, "%s_calendar", tname); save(n);
    key(NZ_KEY_R);
    key(NZ_KEY_START); app_clock_stopwatch_skip(83450); run(100);
    key(NZ_KEY_X); app_clock_stopwatch_skip(41230); key(NZ_KEY_X); app_clock_stopwatch_skip(39870); key(NZ_KEY_X);
    run(200); key(NZ_KEY_START); run(30);
    snprintf(n, sizeof n, "%s_stopwatch", tname); save(n);
    key(NZ_KEY_R); key(NZ_KEY_UP); key(NZ_KEY_UP); run(30);
    snprintf(n, sizeof n, "%s_timer", tname); save(n);
    key(NZ_KEY_START); app_clock_timer_skip(7 * 60000); run(300);
    snprintf(n, sizeof n, "%s_timer_done", tname); save(n);
    close_all();
    run(2000);                                               /* let "time's up!" fade (1.8 s): clean pictures below */

    apps_open(APP_DEVTOOLS); run(50);                        /* the Dev toolbox: a few of its tools */
    snprintf(n, sizeof n, "%s_devtools", tname); save(n);
    for (int i = 0; i < 6; i++) key(NZ_KEY_DOWN);
    run(30);
    snprintf(n, sizeof n, "%s_devtools_colour", tname); save(n);
    key(NZ_KEY_DOWN); run(30);
    snprintf(n, sizeof n, "%s_devtools_json", tname); save(n);
    key(NZ_KEY_DOWN); run(30);
    snprintf(n, sizeof n, "%s_devtools_regex", tname); save(n);
    close_all();

    write_text("/tmp/tools_temp", "47000\n");                /* the Health dashboard (fake readings) */
    apps_open(APP_HEALTH); run(50);
    snprintf(n, sizeof n, "%s_health", tname); save(n);
    close_all();

    /* the same apps in a desktop window (about 478 x 338) */
    ui_set_mode(UI_DESKTOP);
    ui_mouse(0, 0, 0, 0, 0); ui_mouse(NZ_W - 1, NZ_H - 1, 0, 0, 0);   /* pointer to the corner */
    mx = NZ_W - 1; my = NZ_H - 1;
    run(1900);
    static const int ids[] = { APP_TASKMGR, APP_CALCULATOR, APP_NOTES, APP_CLOCK };
    static const char *const names[] = { "taskmgr", "calculator", "notes", "clock" };
    for (int i = 0; i < 4; i++) {
        apps_open(ids[i]); run(60);
        if (ids[i] == APP_CALCULATOR) { type("1250*12\n"); type("(4+8)*2.5\n"); type("355/113"); }
        if (ids[i] == APP_TASKMGR) { fake_stat(150 + 370 + 200, 800 + 630 + 500); app_taskmgr_refresh(); }
        run(60);
        snprintf(n, sizeof n, "%s_window_%s", tname, names[i]); save(n);
        if (ids[i] == APP_CLOCK) {
            KbEvent tab = { KB_TAB, 0 };
            ui_kbd(&tab); run(30);
            snprintf(n, sizeof n, "%s_window_calendar", tname); save(n);
            ui_kbd(&tab); run(30);
            snprintf(n, sizeof n, "%s_window_stopwatch", tname); save(n);
            ui_kbd(&tab); run(30);
            snprintf(n, sizeof n, "%s_window_timer", tname); save(n);
        }
        close_app(ids[i]);
    }
    ui_set_mode(UI_HANDHELD);
    run(50);
    printf("pictures: %s saved\n", tname);
}

/* Switch the theme while each app is open in a window: the apps must re-fit
 * their fonts by themselves (Amber CRT: the 24 px wide heading font). */
static void live_switch_pictures(void)
{
    static const int ids[] = { APP_TASKMGR, APP_CALCULATOR, APP_NOTES, APP_CLOCK };
    static const char *const names[] = { "taskmgr", "calculator", "notes", "clock" };
    char n[64];
    theme_set(TH_ORIGINAL);
    ui_set_mode(UI_DESKTOP);
    run(1900);
    for (int i = 0; i < 4; i++) {
        apps_open(ids[i]); run(60);
        if (ids[i] == APP_CALCULATOR) type("(4+8)*2.5\n");
        theme_set(TH_AMBER_CRT);
        run(1200);                                           /* (Notes looks once a second) */
        snprintf(n, sizeof n, "amber_live_%s", names[i]); save(n);
        theme_set(TH_ORIGINAL);
        close_app(ids[i]);
    }
    ui_set_mode(UI_HANDHELD);
    run(50);
    printf("pictures: live theme switch saved\n");
}

static void setup_sdcard(void)
{
    if (system("rm -rf " NZ_SDCARD " && mkdir -p " NZ_DIR) != 0) {}
    write_file(SETTINGS_FILE, "theme=original\ntz=3\n", 20);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* The memory checks need exact numbers. glibc keeps a few freed blocks in
     * a per-thread cache ("tcache") that still counts as used, which makes the
     * numbers jump around. Start again with that cache switched off. */
    if (!getenv("GLIBC_TUNABLES")) {
        setenv("GLIBC_TUNABLES", "glibc.malloc.tcache_count=0", 1);
        execv("/proc/self/exe", argv);
    }
    int unit_only = argc > 1 && !strcmp(argv[1], "--unit");
    if (argc > 1 && !unit_only) shots = argv[1];
    setup_sdcard();
    hw_apply_timezone(3);                                    /* like the console at start (core.c) */

    test_calc();
    test_procs();
    test_notes_names();
    test_devtools();
    test_health();
    test_wifiinfo();
    test_btinfo();
    if (unit_only) {
        printf("unit checks: %d passed, %d failed\n", passes, fails);
        return fails;
    }

    lv_init();
    nzeus_display_init();
    ui_init();
    apps_register(&app_taskmgr);
    apps_register(&app_calculator);
    apps_register(&app_notes);
    apps_register(&app_clock);
    apps_register(&app_devtools);
    apps_register(&app_health);
    ui_set_status("14:32", 78, 1);
    run(50);

    test_calculator();
    test_calculator_mouse();
    test_taskmgr();
    test_notes();
    test_clock();
    test_devtools_app();
    test_health_app();

    printf("--- memory ---\n");
    make_fake_proc();
    procs_set_root(FAKE_PROC);
    prepare_notes_for_pictures();
    test_leaks(APP_TASKMGR, "Task Manager", 0);
    test_leaks(APP_CALCULATOR, "Calculator", 0);
    test_leaks(APP_NOTES, "Notes", 0);
    test_leaks(APP_CLOCK, "Clock", 0);
    test_leaks(APP_TASKMGR, "Task Manager", 1);
    test_leaks(APP_CALCULATOR, "Calculator", 1);
    test_leaks(APP_NOTES, "Notes", 1);
    test_leaks(APP_CLOCK, "Clock", 1);
    test_leaks(APP_DEVTOOLS, "Dev toolbox", 0);
    test_leaks(APP_DEVTOOLS, "Dev toolbox", 1);
    test_leaks(APP_HEALTH, "Health", 0);
    test_leaks(APP_HEALTH, "Health", 1);

    if (!getenv("NO_PICTURES")) {
        pictures(TH_ORIGINAL, "original");
        pictures(TH_GREEN_CRT, "green");
        pictures(TH_DAYLIGHT, "daylight");
        live_switch_pictures();
        theme_set(TH_ORIGINAL);
    }
    printf("RESULT: %d passed, %d failed\n", passes, fails);
    return fails;
}
