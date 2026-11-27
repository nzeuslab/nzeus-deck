/* wifi.c - Wi-Fi state, scanning and joining (see wifi.h). */
#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bgjob.h"
#include "settings.h"
#include "nzeus_tick.h"
#include "wifi.h"

const char *wifi_sys_root = "";

static BgJob on_job, join_job, status_job, net_job;
static char link_status[96];
static char net_status[200];       /* nzeus-netctl's status line ("iface=wlan0 link=up ip=...") */
static int net_started;
static uint32_t last_status_poll;

static int read_line(const char *path, char *buf, int n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, n, f)) buf[0] = 0;
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

/* --- what is there --- */
int wifi_dongle(void)
{
    char pat[160], vid[8], pid[8];
    int found = DONGLE_NONE;
    glob_t g;
    snprintf(pat, sizeof pat, "%s/sys/bus/usb/devices/*/idVendor", wifi_sys_root);
    if (glob(pat, 0, NULL, &g) != 0) return DONGLE_NONE;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char p[256];
        if (read_line(g.gl_pathv[i], vid, sizeof vid) != 0 || strcmp(vid, "0bda")) continue;   /* Realtek */
        snprintf(p, sizeof p, "%.*sidProduct", (int)(strlen(g.gl_pathv[i]) - 8), g.gl_pathv[i]);
        if (read_line(p, pid, sizeof pid) != 0) continue;
        if (!strcmp(pid, "1a2b")) { if (found == DONGLE_NONE) found = DONGLE_CDROM; }   /* "driver CD" mode */
        else if (!strcmp(pid, "c820") || !strcmp(pid, "c821") || !strcmp(pid, "c811") ||
                 !strcmp(pid, "c812") || !strcmp(pid, "c82a") || !strcmp(pid, "c82b")) found = DONGLE_WIFI;
    }
    globfree(&g);
    return found;
}

/* the driver (rtw88) is built into the kernel: loaded = wlan0 is there */
int wifi_driver_loaded(void)
{
    char w[128];
    snprintf(w, sizeof w, "%s/sys/class/net/wlan0", wifi_sys_root);
    return access(w, F_OK) == 0;
}

int wifi_iface_up(void)
{
    char p[128], v[16];
    snprintf(p, sizeof p, "%s/sys/class/net/wlan0/flags", wifi_sys_root);
    if (read_line(p, v, sizeof v) != 0) return 0;
    return (strtoul(v, NULL, 16) & 1) != 0;                 /* IFF_UP */
}

int wifi_state(void)
{
    if (bgjob_running(&on_job)) return WIFI_STARTING;
    return wifi_iface_up() ? WIFI_ON : WIFI_OFF;
}

const char *wifi_job_log(void)
{
    static char buf[400];
    bgjob_read(&on_job, buf, sizeof buf);
    return buf;
}

/* --- switching on: wlan0 up (nzeus/bin/wifi-on.sh) --- */
int wifi_turn_on(void)
{
    if (bgjob_running(&on_job)) return 0;
    return bgjob_start(&on_job, "sh " NZ_DIR "/bin/wifi-on.sh", "/tmp/nzeus-wifi-on.log");
}

/* --- scanning: nzeus/bin/wifi-scan.sh (core/deck-modern) asks "iw"
 * and writes the networks as Realtek's survey table (below) to this file --- */
#define SURVEY_FILE "/tmp/nzeus-wifi-survey.txt"
static int survey_path(char *out, int n) { snprintf(out, n, "%s", SURVEY_FILE); return 0; }
int wifi_scan_start(void)
{
    if (!wifi_iface_up()) return -1;
    remove(SURVEY_FILE);
    return system("sh " NZ_DIR "/bin/wifi-scan.sh " SURVEY_FILE " > /dev/null 2>&1 &") == 0 ? 0 : -1;
}
/* the script renames its finished list into place: there = done (hardware
 * 2026-10-11: iw needs ~13 s for 2.4 + 5 GHz with the RTL8821CU) */
int wifi_scan_done(void) { return access(SURVEY_FILE, F_OK) == 0; }

static int by_signal(const void *a, const void *b) { return ((const WifiNet *)b)->dbm - ((const WifiNet *)a)->dbm; }

/* The table (the layout of Realtek's survey_info page, rtw_debug.c, which
 * wifi-scan.sh copies):
 *  index  bssid  ch  RSSI  SdBm  Noise  age  flag(32 wide)  ssid(32 wide)
 * Both last columns are right-aligned in 32 characters; the flag looks
 * like "[WPA2][WPS][ESS]", and an SSID may contain spaces. */
int wifi_parse_survey(const char *text, WifiNet *out, int max)
{
    int n = 0;
    const char *line = text;
    while (line && *line && n < max) {
        const char *end = strchr(line, '\n');
        int len = end ? (int)(end - line) : (int)strlen(line);
        char l[256];
        snprintf(l, sizeof l, "%.*s", len < 255 ? len : 255, line);
        line = end ? end + 1 : NULL;
        int idx, ch, rssi, sdbm, noise, age, pos = 0;
        char bssid[24];
        if (sscanf(l, "%d %23s %d %d %d %d %d%n", &idx, bssid, &ch, &rssi, &sdbm, &noise, &age, &pos) != 7) continue;
        if (strlen(bssid) != 17) continue;
        const char *rest = l + pos;
        WifiNet *w = &out[n];
        memset(w, 0, sizeof *w);
        snprintf(w->bssid, sizeof w->bssid, "%s", bssid);
        w->channel = ch;
        w->dbm = sdbm;
        w->secure = strstr(rest, "[WPA") || strstr(rest, "[WEP]");
        /* the SSID: after the last ']' of the flags (or after the spaces when there are no flags) */
        const char *s = strrchr(rest, ']');
        s = s ? s + 1 : rest;
        while (*s == ' ') s++;
        snprintf(w->ssid, sizeof w->ssid, "%s", s);
        size_t sl = strlen(w->ssid);
        while (sl && w->ssid[sl - 1] == ' ') w->ssid[--sl] = 0;
        if (!w->ssid[0]) snprintf(w->ssid, sizeof w->ssid, "(hidden network)");
        int dup = 0;                                         /* the same name on two radios: keep the stronger */
        for (int k = 0; k < n; k++)
            if (!strcmp(out[k].ssid, w->ssid)) { if (w->dbm > out[k].dbm) out[k] = *w; dup = 1; }
        if (!dup) n++;
    }
    qsort(out, n, sizeof *out, by_signal);
    return n;
}

static char survey_text[16384];          /* the last scan list, as wifi-scan.sh wrote it */
const char *wifi_last_survey(void) { return survey_text; }

int wifi_scan_read(WifiNet *out, int max)
{
    char p[200];
    if (survey_path(p, sizeof p) != 0) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t got = fread(survey_text, 1, sizeof survey_text - 1, f);
    survey_text[got] = 0;
    fclose(f);
    return wifi_parse_survey(survey_text, out, max);
}

/* --- joining (wifi-join.sh, built with wpa_supplicant) --- */
static void shell_quote(char *out, size_t n, const char *s)
{
    size_t k = 0;                                            /* 'it'\''s' : safe for any text */
    if (k < n - 1) out[k++] = '\'';
    for (; *s && k < n - 6; s++) {
        if (*s == '\'') { memcpy(out + k, "'\\''", 4); k += 4; }
        else out[k++] = *s;
    }
    out[k++] = '\'';
    out[k] = 0;
}

/* (a status answer asked for before a join could still tell about the old
 * network: it is dropped, and no new one is asked while the join runs) */
int wifi_join(const char *ssid, const char *password)
{
    char cmd[400], qs[120], qp[200];
    if (bgjob_running(&join_job)) bgjob_stop(&join_job);
    bgjob_stop(&status_job);
    shell_quote(qs, sizeof qs, ssid);
    shell_quote(qp, sizeof qp, password ? password : "");
    snprintf(cmd, sizeof cmd, "sh " NZ_DIR "/bin/wifi-join.sh connect %s %s", qs, qp);
    link_status[0] = 0;
    return bgjob_start(&join_job, cmd, "/tmp/nzeus-wifi-join.log");
}

int wifi_join_saved(const char *ssid)
{
    char cmd[300], qs[120];
    if (bgjob_running(&join_job)) bgjob_stop(&join_job);
    bgjob_stop(&status_job);
    shell_quote(qs, sizeof qs, ssid);
    snprintf(cmd, sizeof cmd, "sh " NZ_DIR "/bin/wifi-join.sh use %s", qs);
    link_status[0] = 0;
    return bgjob_start(&join_job, cmd, "/tmp/nzeus-wifi-join.log");
}

int wifi_forget(const char *ssid)
{
    char cmd[300], qs[120];
    shell_quote(qs, sizeof qs, ssid);
    snprintf(cmd, sizeof cmd, "sh " NZ_DIR "/bin/wifi-join.sh forget %s >/dev/null 2>&1", qs);
    return system(cmd) == 0 ? 0 : -1;              /* (quick: it only edits a small file) */
}

/* --- saved networks: read networks.conf ourselves (no program to start) ---
 * wifi-join.sh writes each name as hex ("	ssid=486f6d65" = "Home"), so any
 * character in a name is kept exactly. */
const char *wifi_saved_file = NZ_DIR "/wifi/networks.conf";

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int wifi_saved_list(char (*names)[33], int max)
{
    FILE *f = fopen(wifi_saved_file, "r");
    char line[200];
    int n = 0;
    if (!f) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "ssid=", 5)) continue;
        p += 5;
        int k = 0;
        while (k < 32 && hex_digit(p[0]) >= 0 && hex_digit(p[1]) >= 0) {   /* two hex digits = one letter */
            names[n][k++] = (char)(hex_digit(p[0]) * 16 + hex_digit(p[1]));
            p += 2;
        }
        names[n][k] = 0;
        if (k) n++;
    }
    fclose(f);
    return n;
}

int wifi_is_saved(const char *ssid)
{
    char names[16][33];
    int n = wifi_saved_list(names, 16);
    for (int i = 0; i < n; i++) if (!strcmp(names[i], ssid)) return 1;
    return 0;
}

int wifi_disconnect(void)
{
    link_status[0] = 0;
    return system("sh " NZ_DIR "/bin/wifi-join.sh disconnect >/dev/null 2>&1") == 0 ? 0 : -1;
}

int wifi_join_running(void) { return bgjob_running(&join_job); }
int wifi_join_failed(void) { return !bgjob_running(&join_job) && join_job.exit_code > 0; }

const char *wifi_link_status(void) { return link_status; }
const char *wifi_net_status(void) { return net_status; }

/* the IP address from nzeus-netctl's status line, or "" */
const char *wifi_ip(void)
{
    static char ip[20];
    const char *p = strstr(net_status, " ip=");
    ip[0] = 0;
    if (p) sscanf(p + 4, "%19[0-9.]", ip);
    if (!strcmp(ip, "0.0.0.0")) ip[0] = 0;
    return ip;
}

void wifi_poll(void)
{
    bgjob_poll(&on_job);
    bgjob_poll(&join_job);
    bgjob_poll(&net_job);
    if (bgjob_poll(&status_job) == 0 && status_job.exit_code >= 0) {   /* a status answer came back */
        char buf[400];
        bgjob_read(&status_job, buf, sizeof buf);
        char *second = strchr(buf, '\n');                 /* line 1: Wi-Fi link, line 2: the address (netctl) */
        if (second) *second++ = 0;
        buf[strcspn(buf, "\r")] = 0;
        snprintf(link_status, sizeof link_status, "%.95s", buf);
        net_status[0] = 0;
        /* nzeus-netctl prints "iface=wlan0 link=up ip=192.168.1.57", or "daemon=none link=down ip=none" */
        if (second && strstr(second, "link=up") && strstr(second, " ip=") && !strstr(second, " ip=none")) {
            second[strcspn(second, "\r\n")] = 0;
            snprintf(net_status, sizeof net_status, "%.199s", second);
        }
        status_job.exit_code = -1;
        /* joined, but no address yet: nzeus-net-up.sh starts the DHCP client (once per join) */
        if (!strncmp(link_status, "state=COMPLETED", 15) && !net_status[0] && !net_started && !bgjob_running(&net_job)) {
            net_started = 1;
            bgjob_start(&net_job, "sh " NZ_DIR "/bin/nzeus-net-up.sh", "/tmp/nzeus-net-up.log");
        }
        if (strncmp(link_status, "state=COMPLETED", 15)) net_started = 0;   /* (left the network: next join starts it again) */
    }
    uint32_t now = nzeus_tick_ms();
    if (wifi_iface_up() && !bgjob_running(&status_job) && !bgjob_running(&join_job) && now - last_status_poll > 2000) {
        last_status_poll = now;                              /* ask again every 2 s */
        bgjob_start(&status_job, "sh " NZ_DIR "/bin/wifi-join.sh status; " NZ_DIR "/bin/nzeus-netctl status 2>/dev/null",
                    "/tmp/nzeus-wifi-status.log");
    }
}
