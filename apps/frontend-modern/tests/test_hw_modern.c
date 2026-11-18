/* test_hw_modern.c - PC test of hw_modern.c (the new kernel's battery,
 * charger and backlight files), with fake files in /tmp/nzhwtest.
 * Built by `build_nzeus_modern.sh test` with the PC's gcc and -DNZ_TESTING
 * (so nothing here can ever switch the PC off). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"

#define DIR "/tmp/nzhwtest"
static int failures;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int file_is(const char *path, const char *text)
{
    char buf[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    return !strcmp(buf, text);
}

int main(void)
{
    if (system("rm -rf " DIR " && mkdir -p " DIR) != 0) return 1;
    hw_backlight_file = DIR "/brightness";
    hw_backlight_glob = DIR "/bl/*";
    hw_ctl_dev = DIR "/controlC0";              /* (no sound card on the PC) */
    hw_pcm_dev = DIR "/pcmC0D0p";
    hw_headphones_file = DIR "/headphones";
    hw_capacity_file = DIR "/capacity";
    hw_charger_file = DIR "/online";
    hw_levels_file = DIR "/stock_levels";
    hw_power_driver = DIR "/driver";

    check(hw_battery_percent() == -1 && hw_battery_raw() == -1 && hw_battery_charging() == 0,
          "no driver (today's kernel): battery unknown, not charging");
    check(hw_set_brightness(50) == -1, "no backlight driver: brightness says not possible");
    /* the speaker switch (the sound driver's "headphones" file) */
    check(hw_get_speaker() == -1 && hw_set_speaker(0) == -1, "no sound driver: no speaker switch");
    {
        FILE *h = fopen(DIR "/headphones", "w");
        if (h) { fputs("level=1 plugged_level=-1 in=0 amp=0\n", h); fclose(h); }
        check(hw_get_speaker() == 1, "speaker switch: in=0 (no headphones) = speaker on");
        check(hw_set_speaker(0) == 0 && file_is(DIR "/headphones", "1\n"),
              "speaker off: writes the pin's present level (1) = 'headphones in' = amp off");
        h = fopen(DIR "/headphones", "w");
        if (h) { fputs("level=0 plugged_level=-1 in=0 amp=0\n", h); fclose(h); }
        check(hw_set_speaker(0) == 0 && file_is(DIR "/headphones", "0\n"), "speaker off with the pin at 0: writes 0");
        h = fopen(DIR "/headphones", "w");
        if (h) { fputs("level=1 plugged_level=1 in=1 amp=0\n", h); fclose(h); }
        check(hw_get_speaker() == 0, "speaker switch: in=1 = speaker off");
        check(hw_set_speaker(1) == 0 && file_is(DIR "/headphones", "off\n"), "speaker on: writes off (pin ignored)");
        h = fopen(DIR "/headphones", "w");
        if (h) { fputs("no headphone pin described\n", h); fclose(h); }
        check(hw_get_speaker() == -1 && hw_set_speaker(0) == -1, "a driver without the pin: no speaker switch");
    }
    check(hw_modern_power_off() == -1, "no power driver: power-off says not possible (NZEUS goes on)");

    put(DIR "/stock_levels", "ref=247 battery=172 charger=0\n");
    check(hw_battery_raw() == 172, "stock_levels: the battery reading (172)");
    check(hw_battery_percent() == hw_battery_from_adc(172), "no capacity file: % from NZEUS's own line");
    check(hw_battery_charging() == 0, "charger=0: not charging");
    put(DIR "/stock_levels", "ref=247 battery=171 charger=138\n");
    check(hw_battery_charging() == 1, "no online file: charger=138 (>= 64) means plugged in");
    put(DIR "/stock_levels", "ref=0 battery=40 charger=0\n");
    check(hw_battery_raw() == -1 && hw_battery_percent() == -1,
          "an unbelievable reading (40) is ignored, not taken as an empty battery");

    put(DIR "/capacity", "87\n");
    put(DIR "/online", "0\n");
    put(DIR "/stock_levels", "ref=247 battery=172 charger=138\n");
    check(hw_battery_percent() == 87, "the kernel's capacity file wins (87 %)");
    check(hw_battery_charging() == 0, "the kernel's charger/online file wins (0)");
    put(DIR "/online", "1\n");
    check(hw_battery_charging() == 1, "charger/online = 1: charging");

    put(DIR "/brightness", "");
    check(hw_set_brightness(100) == 0 && file_is(DIR "/brightness", "255\n"), "brightness 100 % -> 255");
    check(hw_set_brightness(20) == 0 && file_is(DIR "/brightness", "43\n"), "brightness 20 % -> 43 (the stock curve)");
    /* no r36s-avp, but another backlight (e.g. a native PWM one, max 100): scaled */
    remove(DIR "/brightness");
    if (system("mkdir -p " DIR "/bl/pwm && echo 100 > " DIR "/bl/pwm/max_brightness") != 0) return 1;
    put(DIR "/bl/pwm/brightness", "");
    check(hw_set_brightness(100) == 0 && file_is(DIR "/bl/pwm/brightness", "100\n"), "other backlight: 100 % -> its max (100)");
    check(hw_set_brightness(20) == 0 && file_is(DIR "/bl/pwm/brightness", "16\n"), "other backlight: 20 % -> 43/255 of it (16)");
    /* pure Linux (hardware 2026-10-11): r36s-avp is there but refuses (EPERM, the AVP is off):
     * the other backlight must be set, and the Deck must not say "no backlight device" */
    if (system("mkdir -p " DIR "/brightness") != 0) return 1;   /* (a folder: every write to it fails) */
    put(DIR "/bl/pwm/brightness", "");
    check(hw_set_brightness(100) == 0 && file_is(DIR "/bl/pwm/brightness", "100\n"), "r36s-avp refuses: the other backlight is set");
    check(hw_set_volume(50) == -1 && hw_get_volume() == -1, "no sound card: volume says not possible");
    { int16_t f[4] = { 1, 2, 3, 4 };
      check(hw_pcm_write(f, 2) == 2, "no sound card: sound is taken and dropped (NZEUS goes on)"); }

    put(DIR "/driver", "");
    check(hw_modern_power_off() == -1, "test build: never switches off, even with a power driver");

    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
