#ifndef NZEUS_WIFI_H
#define NZEUS_WIFI_H

/* Wi-Fi on the SD Debian (Linux 7.2):
 *   - the LB-Link dongle (Realtek RTL8821CU, USB 0bda:c820), driver rtw88
 *     built into the kernel; it needs Settings > USB on Devices
 *   - the helpers in nzeus/bin (core/deck-modern): wifi-on.sh switches
 *     wlan0 on, wifi-scan.sh scans with "iw", wifi-join.sh joins with
 *     wpa_supplicant, nzeus-net-up.sh gets an address (DHCP), nzeus-netctl
 *     says which address wlan0 has
 * Everything slow (switching on, joining) runs as a background program, so
 * the screen never freezes. */

enum { DONGLE_NONE, DONGLE_CDROM, DONGLE_WIFI };
enum { WIFI_OFF, WIFI_STARTING, WIFI_ON };

typedef struct {
    char ssid[33];
    char bssid[18];
    int channel;
    int dbm;          /* signal: -40 strong ... -90 weak */
    int secure;       /* 1 = needs a password (WPA/WPA2/WEP) */
} WifiNet;

int  wifi_dongle(void);                 /* DONGLE_* */
int  wifi_driver_loaded(void);
int  wifi_iface_up(void);               /* wlan0 exists and is switched on */
int  wifi_state(void);                  /* WIFI_* (also checks the background job) */
const char *wifi_job_log(void);         /* the last lines the background job printed */

int  wifi_turn_on(void);                /* starts nzeus/bin/wifi-on.sh in the background */
int  wifi_scan_start(void);             /* starts wifi-scan.sh in the background; 0 = started */
int  wifi_scan_done(void);              /* 1 = the scan list is ready (/tmp/nzeus-wifi-survey.txt) */
int  wifi_scan_read(WifiNet *out, int max);   /* the networks found, strongest first */
int  wifi_parse_survey(const char *text, WifiNet *out, int max);   /* (tests) */
const char *wifi_last_survey(void);     /* the last scan list as wifi-scan.sh wrote it (for the ID cards, wifiinfo.c) */

int  wifi_join(const char *ssid, const char *password);   /* starts wifi-join.sh connect (saves it) */
int  wifi_join_saved(const char *ssid);   /* starts wifi-join.sh use: a saved network, no password */
int  wifi_join_running(void);             /* 1 while wifi-join.sh (connect / use) still works */
int  wifi_join_failed(void);              /* 1 = it ended with an error (bad password length, no dongle...) */
int  wifi_forget(const char *ssid);       /* wifi-join.sh forget: deletes a saved network; 0 = done */
int  wifi_saved_list(char (*names)[33], int max);   /* the saved networks' names; returns how many */
int  wifi_is_saved(const char *ssid);
int  wifi_disconnect(void);
const char *wifi_link_status(void);     /* "COMPLETED ssid=..." etc, or "" (polled) */
const char *wifi_net_status(void);      /* nzeus-netctl's "iface=wlan0 link=up ip=..." line, or "" */
const char *wifi_ip(void);              /* our IP address once the internet is up, or "" */
void wifi_poll(void);                   /* every frame: finishes background jobs */

/* Where things are (tests point these at fake folders). */
extern const char *wifi_sys_root;       /* "" -> /sys/... */
extern const char *wifi_saved_file;     /* nzeus/wifi/networks.conf (made by wifi-join.sh) */

#endif
