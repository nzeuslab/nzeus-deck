/* usbmode.c - Settings > USB: runs nzeus-usbmode.sh in the background (see usbmode.h). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "bgjob.h"
#include "nzeus_tick.h"
#include "settings.h"
#include "usbmode.h"

#define USB_SH NZ_DIR "/bin/nzeus-usbmode.sh"

static BgJob st_job, sw_job;                    /* "status" every 3 s; a switch */
static int state = USBM_CHECKING, ask_now = 1;
static uint32_t last_ask;
static char line[96], note[128];

/* the value of "key=" in the status answer (one fact per line), "" if missing */
static void value_of(const char *text, const char *key, char *out, int n)
{
    size_t k = strlen(key);
    out[0] = 0;
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, k) && p[k] == '=') {
            int i = 0;
            for (p += k + 1; *p && *p != '\n' && i < n - 1; p++) out[i++] = *p;
            out[i] = 0;
            return;
        }
}

/* the status answer -> state + one line in plain words. nzeus-usbmode.sh
 * status prints one fact per line: mode=pc|host, then for PC mode pc=
 * (connected...) and ip=, for Devices mode devices= (how many), and error=
 * when something is wrong. */
static void read_status(void)
{
    char buf[700], mode[8], pc[40], ip[20], dev[8], err[100];
    bgjob_read(&st_job, buf, sizeof buf);
    value_of(buf, "mode", mode, sizeof mode);
    if (!mode[0]) { state = USBM_MISSING; snprintf(line, sizeof line, "not installed"); return; }
    value_of(buf, "pc", pc, sizeof pc);       value_of(buf, "ip", ip, sizeof ip);
    value_of(buf, "devices", dev, sizeof dev);
    value_of(buf, "error", err, sizeof err);
    int has_ip = ip[0] && strcmp(ip, "none");
    if (!strcmp(mode, "pc")) {
        state = USBM_PC;
        snprintf(line, sizeof line, "PC: %s, network %s",
                 strncmp(pc, "connected", 9) ? "waiting for the cable" : "connected", has_ip ? ip : "no address yet");
    } else {
        state = USBM_HOST;
        snprintf(line, sizeof line, "Devices: %s plugged in", dev[0] ? dev : "0");
    }
    if (err[0]) snprintf(note, sizeof note, "Problem: %s", err);
}

void usbmode_refresh(void) { ask_now = 1; }

void usbmode_poll(void)
{
    if (bgjob_running(&sw_job) && !bgjob_poll(&sw_job)) {      /* a switch has just finished */
        char out[300], *l;
        bgjob_read(&sw_job, out, sizeof out);
        while ((l = strrchr(out, '\n')) && !l[1]) *l = 0;      /* its last line: what to tell the user */
        l = strrchr(out, '\n');
        l = l ? l + 1 : out;
        if (!strncmp(l, "ERROR: ", 7)) snprintf(note, sizeof note, "Problem: %.110s", l + 7);
        else snprintf(note, sizeof note, "%.120s", l);
        state = USBM_CHECKING;                                 /* until the next status answer */
        ask_now = 1;
    }
    if (bgjob_running(&st_job)) {
        if (bgjob_poll(&st_job)) return;                       /* still asking */
        if (!bgjob_running(&sw_job)) read_status();
    }
    uint32_t now = nzeus_tick_ms();
    if (bgjob_running(&sw_job) || (!ask_now && now - last_ask < 3000)) return;
    ask_now = 0;
    last_ask = now;
    if (access(USB_SH, R_OK) != 0) { state = USBM_MISSING; snprintf(line, sizeof line, "not installed"); return; }
    bgjob_start(&st_job, "sh " USB_SH " status", "/tmp/nzeus-usbmode.out");
}

int usbmode_switch(int to_pc)
{
    if (bgjob_running(&sw_job) || access(USB_SH, R_OK) != 0) return -1;
    bgjob_stop(&st_job);                                       /* (no status while switching) */
    note[0] = 0;
    if (bgjob_start(&sw_job, to_pc ? "sh " USB_SH " pc" : "sh " USB_SH " host", "/tmp/nzeus-usbmode-switch.out")) return -1;
    return 0;
}

int usbmode_state(void) { return bgjob_running(&sw_job) ? USBM_SWITCHING : state; }
const char *usbmode_line(void) { return state == USBM_CHECKING ? "checking..." : line; }
const char *usbmode_note(void) { return note; }
