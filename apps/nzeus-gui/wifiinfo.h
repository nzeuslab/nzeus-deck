#ifndef NZEUS_WIFIINFO_H
#define NZEUS_WIFIINFO_H
#include <stddef.h>

/* Wi-Fi "ID cards": everything the deck can tell about the networks around,
 * from the scan list (wifi-scan.sh writes it in Realtek's survey_info layout).
 * Learned from the console's REAL scans (reports/lab18, lab20, lab20b):
 *   - the RTL8821CU driver writes [WEP] next to [WPA2] for normal WPA2
 *     networks, so [WEP] alone is not "unsafe"
 *   - one router shows up once per band with nearly the same address
 *     (e.g. 82:11:22:33:44:90 on channel 3, ...:94 on channel 36): the same router,
 *     not a fake twin
 *   - many routers use a "private" (locally made) address: no maker in it */

typedef struct {
    char ssid[33];            /* "" = a hidden network */
    char bssid[18];
    int  channel, dbm;
    char flags[64];           /* "[WPA2][WEP][WPS][ESS]" */
} WiNet;

int  wi_parse(const char *survey, WiNet *out, int max);   /* every row, in the driver's order */
const char *wi_band(int channel);                         /* "2.4 GHz", "5 GHz", or "?" */
int  wi_security(const char *flags, char *out, size_t n); /* 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA3 (+ words) */
int  wi_wps(const char *flags);                           /* WPS switched on */
int  wi_private_mac(const char *bssid);                   /* a locally made address (no maker in it) */
int  wi_same_router(const char *a, const char *b);        /* two addresses of one router (bands / extra networks) */
void wi_signal(int dbm, int channel, char *out, size_t n);   /* bars + words + a rough distance */
/* the maker from the IEEE list on the card ("AABBCC<tab or space>Maker" lines):
 * 0 = found, -1 = private address, -2 = not in the list / no list */
int  wi_maker(const char *bssid, const char *list_file, char *out, size_t n);
/* the best 2.4 GHz channel (1, 6 or 11) for a router, ignoring the network
 * called `mine` (yours, NULL = none); why: one line on how crowded each is */
int  wi_best_channel(const WiNet *nets, int n, const char *mine, char *why, size_t wn);
/* networks with the same name from a DIFFERENT router (worth a look: a fake?) */
int  wi_twins(const WiNet *nets, int n, int i);
/* the full ID card of network i */
void wi_card(const WiNet *nets, int n, int i, const char *list_file, char *out, size_t sz);

#endif
