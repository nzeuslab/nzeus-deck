/* wifiinfo.c - Wi-Fi "ID cards" for the deck: band, channel, security in
 * plain words, WPS, signal and a rough distance, the router's maker, the best
 * channel for your own router, and same-name networks worth a second look.
 * Works on the driver's scan list; see wifiinfo.h for what the console's real
 * scans taught us. No screen code here (app_wifi.c shows the cards). */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifiinfo.h"

int wi_parse(const char *survey, WiNet *out, int max)
{
    int n = 0;
    const char *line = survey;
    while (line && *line && n < max) {
        const char *end = strchr(line, '\n');
        int len = end ? (int)(end - line) : (int)strlen(line);
        char l[256], bssid[24];
        snprintf(l, sizeof l, "%.*s", len < 255 ? len : 255, line);
        line = end ? end + 1 : NULL;
        int idx, ch, rssi, sdbm, noise, age, pos = 0;
        /* " 1  aa:bb:cc:dd:ee:01   6   60  -52   0  120   [WPA2][WPS][ESS]   NZEUS LAB" */
        if (sscanf(l, "%d %23s %d %d %d %d %d%n", &idx, bssid, &ch, &rssi, &sdbm, &noise, &age, &pos) != 7) continue;
        if (strlen(bssid) != 17) continue;
        WiNet *w = &out[n++];
        memset(w, 0, sizeof *w);
        snprintf(w->bssid, sizeof w->bssid, "%s", bssid);
        w->channel = ch;
        w->dbm = sdbm;
        const char *rest = l + pos;
        while (*rest == ' ') rest++;
        const char *close = strrchr(rest, ']');             /* the flags: from the first [ to the last ] */
        if (*rest == '[' && close) {
            snprintf(w->flags, sizeof w->flags, "%.*s", (int)(close - rest + 1), rest);
            rest = close + 1;
        }
        while (*rest == ' ') rest++;
        snprintf(w->ssid, sizeof w->ssid, "%s", rest);
        size_t sl = strlen(w->ssid);
        while (sl && w->ssid[sl - 1] == ' ') w->ssid[--sl] = 0;
    }
    return n;
}

const char *wi_band(int ch)
{
    if (ch >= 1 && ch <= 14) return "2.4 GHz";
    if (ch >= 32 && ch <= 177) return "5 GHz";
    return "?";
}

int wi_security(const char *flags, char *out, size_t n)
{
    /* (this driver writes [WEP] next to [WPA2] for normal WPA2 networks:
     *  WEP only counts when there is no WPA at all) */
    int g;
    if (strstr(flags, "WPA3") || strstr(flags, "SAE")) { g = 4; snprintf(out, n, "WPA3 - strong"); }
    else if (strstr(flags, "WPA2") || strstr(flags, "RSN")) { g = 3; snprintf(out, n, "WPA2 - good"); }
    else if (strstr(flags, "[WPA")) { g = 2; snprintf(out, n, "WPA (the old one) - weak, can be broken"); }
    else if (strstr(flags, "WEP")) { g = 1; snprintf(out, n, "WEP - unsafe, broken in minutes"); }
    else { g = 0; snprintf(out, n, "Open - no password, others can see the traffic"); }
    return g;
}

int wi_wps(const char *flags) { return strstr(flags, "[WPS]") != NULL; }

/* the address as 6 numbers; 0 = ok */
static int mac_bytes(const char *s, unsigned b[6])
{
    return sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ? 0 : -1;
}

int wi_private_mac(const char *bssid)
{
    unsigned b[6];
    return mac_bytes(bssid, b) == 0 && (b[0] & 0x02);       /* the "locally administered" bit */
}

int wi_same_router(const char *a, const char *b)
{
    unsigned x[6], y[6];
    if (mac_bytes(a, x) || mac_bytes(b, y)) return 0;
    /* one router, several networks/bands: the same address except the last
     * number (82:..:83:90 / 82:..:83:94), or except the "local" bit of the
     * first one (48:55:66:.. / 4a:55:66:..) */
    int first_ok = (x[0] | 0x02) == (y[0] | 0x02);
    int mid_ok = x[1] == y[1] && x[2] == y[2] && x[3] == y[3] && x[4] == y[4];
    int last_ok = abs((int)x[5] - (int)y[5]) <= 16;
    return first_ok && mid_ok && (last_ok || x[0] != y[0]);
}

void wi_signal(int dbm, int channel, char *out, size_t n)
{
    int bars = dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : 1;
    static const char *const word[] = { "", "weak", "fair", "good", "excellent" };
    /* rough distance: about -40 dBm at 1 m (2.4 GHz; -47 at 5 GHz), losing
     * 30 dB for every 10x further indoors. Walls change it a lot. */
    double ref = channel > 14 ? -47.0 : -40.0;
    double m = pow(10.0, (ref - dbm) / 30.0);
    if (m < 1) m = 1;
    snprintf(out, n, "%d dBm, %s (%d of 4 bars), about %.0f m away", dbm, word[bars], bars, m);
}

int wi_maker(const char *bssid, const char *list_file, char *out, size_t n)
{
    unsigned b[6];
    if (mac_bytes(bssid, b)) { snprintf(out, n, "?"); return -2; }
    if (b[0] & 0x02) { snprintf(out, n, "hidden (the router uses a private address)"); return -1; }
    char want[32], line[160];
    snprintf(want, sizeof want, "%02X%02X%02X", b[0], b[1], b[2]);
    FILE *f = list_file ? fopen(list_file, "r") : NULL;
    if (!f) { snprintf(out, n, "unknown (no maker list on the card yet)"); return -2; }
    while (fgets(line, sizeof line, f)) {
        char hex[7] = "";
        int k = 0;                                           /* "AABBCC", "AA-BB-CC" or "AA:BB:CC" at the start */
        for (const char *p = line; *p && k < 6; p++) {
            if (isxdigit((unsigned char)*p)) hex[k++] = (char)toupper((unsigned char)*p);
            else if (*p != '-' && *p != ':') break;
        }
        hex[k] = 0;
        if (k == 6 && !strcmp(hex, want)) {
            const char *name = line;
            while (*name && !isspace((unsigned char)*name)) name++;   /* skip the number */
            while (*name && isspace((unsigned char)*name)) name++;
            if (!strncmp(name, "(hex)", 5)) name += 5;                /* (the IEEE file's own layout) */
            while (*name && isspace((unsigned char)*name)) name++;
            snprintf(out, n, "%s", name);
            out[strcspn(out, "\r\n")] = 0;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    snprintf(out, n, "unknown (not in the maker list)");
    return -2;
}

int wi_best_channel(const WiNet *nets, int n, const char *mine, char *why, size_t wn)
{
    static const int choices[3] = { 1, 6, 11 };
    int score[3] = { 0, 0, 0 }, count[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        if (nets[i].channel < 1 || nets[i].channel > 14) continue;    /* only 2.4 GHz overlaps like this */
        if (mine && nets[i].ssid[0] && !strcmp(nets[i].ssid, mine)) continue;
        for (int c = 0; c < 3; c++)
            if (abs(nets[i].channel - choices[c]) <= 4) {            /* within 4 channels = they overlap */
                int loud = nets[i].dbm + 100;                         /* -40 dBm counts 60, -90 counts 10 */
                score[c] += loud > 0 ? loud : 1;
                count[c]++;
            }
    }
    int best = 0;
    for (int c = 1; c < 3; c++) if (score[c] < score[best]) best = c;
    snprintf(why, wn, "Channel 1: %d network%s, 6: %d, 11: %d nearby", count[0], count[0] == 1 ? "" : "s", count[1], count[2]);
    return choices[best];
}

int wi_twins(const WiNet *nets, int n, int i)
{
    int twins = 0;
    if (!nets[i].ssid[0]) return 0;                          /* hidden networks have no name to copy */
    for (int k = 0; k < n; k++)
        if (k != i && !strcmp(nets[k].ssid, nets[i].ssid) && !wi_same_router(nets[k].bssid, nets[i].bssid)) twins++;
    return twins;
}

void wi_card(const WiNet *nets, int n, int i, const char *list_file, char *out, size_t sz)
{
    const WiNet *w = &nets[i];
    char sec[80], sig[96], maker[96];
    size_t k = 0;
    wi_security(w->flags, sec, sizeof sec);
    wi_signal(w->dbm, w->channel, sig, sizeof sig);
    wi_maker(w->bssid, list_file, maker, sizeof maker);
#define ADD(...) do { if (k < sz) k += (size_t)snprintf(out + k, sz - k, __VA_ARGS__); } while (0)
    ADD("Name      %s\n", w->ssid[0] ? w->ssid : "(hidden - it doesn't send its name)");
    ADD("Address   %s\n", w->bssid);
    ADD("Maker     %s\n", maker);
    ADD("Band      %s, channel %d\n", wi_band(w->channel), w->channel);
    ADD("Signal    %s\n", sig);
    ADD("Security  %s\n", sec);
    ADD("WPS       %s", wi_wps(w->flags) ? "on - a weak spot (its 8-digit PIN can be guessed)" : "off");
    int others = 0;                                          /* the same router's other networks/bands */
    for (int j = 0; j < n; j++)
        if (j != i && wi_same_router(nets[j].bssid, w->bssid)) {
            if (!others++) ADD("\nSame router  ");
            else ADD(", ");
            ADD("%s (ch %d)", nets[j].ssid[0] ? nets[j].ssid : "hidden", nets[j].channel);
        }
    int t = wi_twins(nets, n, i);
    if (t) ADD("\nCheck     %d other router%s use%s this name too - make sure it's the real one",
               t, t == 1 ? "" : "s", t == 1 ? "s" : "");
#undef ADD
}
