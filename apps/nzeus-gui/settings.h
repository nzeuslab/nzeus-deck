#ifndef NZEUS_SETTINGS_H
#define NZEUS_SETTINGS_H

/* NZEUS's saved settings: one small text file in NZEUS's folder,
 * nzeus/settings.ini, with one "key=value" per line, e.g.
 *     theme=green_crt
 *     repeat_ms=120
 * Every change is written straight away, so it survives a reboot or a
 * flat battery. */

/* NZ_SDCARD: NZEUS's own folder. On the SD Debian /tmp/nzeus-data is a link
 * to /data/nzeus/deck (the card's Linux data partition); tests on the PC use
 * a fake folder. (The name is older than the Linux system: it was the SD card.) */
#ifndef NZ_SDCARD
#define NZ_SDCARD "/tmp/nzeus-data"
#endif
#define NZ_DIR        NZ_SDCARD "/nzeus"
#define SETTINGS_FILE NZ_DIR "/settings.ini"

const char *settings_get(const char *key, const char *fallback);  /* the value, or fallback */
int  settings_get_int(const char *key, int fallback);
void settings_set(const char *key, const char *value);            /* saves to the SD card */
void settings_set_int(const char *key, int value);
void settings_reload(void);                                       /* read the file again */

/* Settings > Controls: B confirms and A goes back (ui_key swaps them) */
int settings_accept_is_b(void);

#endif
