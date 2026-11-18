/* pc_preview.c - runs NZEUS on the PC and saves pictures of every screen in
 * every theme, so each one can be checked before it goes onto the console.
 * Uses the same UI code as the console version; buttons and the mouse are
 * simulated.
 * Usage: pc_preview OUTPUT_FOLDER [PYTHON_FILE]
 * Pictures: OUTPUT_FOLDER/t<theme>_<screen>.ppm  (e.g. t1_home.ppm = Green CRT home) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "display.h"
#include "home.h"
#include "nzui.h"
#include "ui.h"
#include "desktop.h"
#include "nzeus_tick.h"
#include "theme.h"
#include "wifi.h"
#include "obd.h"

int app_editor_open_file(const char *path);     /* app_editor.c */
void app_terminal_run(const char *cmd);         /* app_terminal.c */
static const char *dir = ".";
static const char *pyfile;

/* Save the current picture as a PPM file (a very simple picture format). */
static void save(int th, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/t%d_%s.ppm", dir, th, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6 %d %d 255\n", NZ_W, NZ_H);
    for (int i = 0; i < NZ_W * NZ_H; i++) {
        uint16_t p = nzeus_fb[i];                       /* RGB565 -> 3 bytes */
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* let NZEUS run for ms milliseconds of real time (timers, apps, pointer reads) */
static void run(int ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)ms;
    while ((int32_t)(end - nzeus_tick_ms()) > 0) { ui_tick(); lv_timer_handler(); usleep(5000); }
    lv_refr_now(NULL);
}

/* simulated USB mouse: move to (x, y), then click there */
static int mx = NZ_W / 2, my = NZ_H / 2;
static void mouse_move(int x, int y) { mx = x; my = y; ui_mouse(mx, my, 0, 0, 1); run(60); }
static void mouse_click(void) { ui_mouse(mx, my, 1, 1, 1); run(60); ui_mouse(mx, my, 0, 0, 1); run(120); }
static void key(int k) { ui_key(k); run(30); }

/* console-like numbers for the System window (the PC's own would confuse) */
static void fake_sysinfo(char *buf, size_t size)
{
    snprintf(buf, size, "Kernel:  7.2.0-rc5-nzeus\nMemory:  64 MB free of 115 MB\nSwap:  14 MB in use\n"
                        "SD card:  36.2 GB free of 39.1 GB\nWi-Fi:  on, not connected\nUptime:  0h 05m");
}

static void close_all_apps(void)
{
    for (int id = 0; id < APP_ALL_COUNT; id++) { NzApp *a = apps_get(id); if (a && a->root) apps_close(a); }
    run(50);
}

/* Every screen, in the theme in use. */
static void render_all(int th)
{
    ui_set_mode(UI_HANDHELD);
    close_all_apps();
    run(1900);                                  /* let messages disappear */
    save(th, "home");

    apps_open(APP_SETTINGS); run(100);
    save(th, "settings");
    ui_key(NZ_KEY_DOWN); ui_key(NZ_KEY_DOWN); ui_key(NZ_KEY_DOWN); run(100);   /* Date & time */
    save(th, "settings_time");
    close_all_apps();

    apps_open(APP_PYTHON); run(100);
    save(th, "python");
    close_all_apps();

    /* every other app, as it first looks */
    static const struct { int id; const char *name; int wait; } more[] = {
        { APP_WIFI, "wifi", 400 }, { APP_MEDIA, "media", 300 }, { APP_ARDUINO, "arduino", 200 },
        { APP_CAR, "car", 600 }, { APP_CLAUDE, "claude", 200 }, { APP_WEB, "web", 200 },
        { APP_ELECTRONICS, "electronics", 200 }, { APP_TASKMGR, "taskmgr", 300 },
        { APP_CALCULATOR, "calculator", 200 }, { APP_NOTES, "notes", 200 }, { APP_CLOCK, "clock", 200 },
        { APP_IMAGES, "images", 200 },
    };
    for (size_t i = 0; i < sizeof more / sizeof more[0]; i++) {
        apps_open(more[i].id); run(more[i].wait);
        if (more[i].id == APP_WIFI) {                       /* show a scan result */
            ui_key(NZ_KEY_A); run(50);                       /* wifi-scan.sh's list is there */
            if (system("cp /tmp/nz_preview_survey.txt /tmp/nzeus-wifi-survey.txt") != 0) {}
            run(500);
        }
        if (more[i].id == APP_MEDIA) { ui_key(NZ_KEY_A); run(1200); }   /* play the first song */
        if (more[i].id == APP_ELECTRONICS) { save(th, more[i].name); ui_key(NZ_KEY_A); run(200); save(th, "electronics_resistor"); close_all_apps(); continue; }
        if (more[i].id == APP_CLOCK) { save(th, more[i].name); ui_key(NZ_KEY_R); run(200); save(th, "clock_calendar"); close_all_apps(); continue; }
        if (more[i].id == APP_IMAGES) { save(th, more[i].name); ui_key(NZ_KEY_A); run(300); save(th, "images_view"); close_all_apps(); continue; }
        save(th, more[i].name);
        close_all_apps();
    }

    apps_open(APP_FILES); run(100);
    save(th, "files");
    close_all_apps();

    app_terminal_run("ls -la /tmp/nz_preview_sd; echo; echo 'Hello from NZEUS Deck'; uname -sr");
    run(600);
    save(th, "terminal");
    key(NZ_KEY_A); run(100);                    /* the on-screen keyboard */
    save(th, "terminal_keyboard");
    close_all_apps();

    if (pyfile) {
        app_editor_open_file(pyfile);
        for (int i = 0; i < 10; i++) ui_key(NZ_KEY_DOWN);
        for (int i = 0; i < 18; i++) ui_key(NZ_KEY_RIGHT);
        run(50);
        save(th, "editor");
        close_all_apps();
    }

    static const char *const items[] = { "Copy", "Cut", "Paste here", "Rename", "Delete", "New folder" };
    nz_menu("Actions", items, 6, NULL, NULL); run(50);
    save(th, "popup_menu");
    nzui_key(NZ_KEY_B); run(50);
    nz_confirm("Delete hello.py?", NULL, NULL); run(50);
    save(th, "popup_confirm");
    nzui_key(NZ_KEY_B); run(50);

    ui_mouse(mx, my, 0, 0, 1); run(1900);       /* a mouse is used: desktop mode */
    save(th, "desktop");
    mouse_move(8 + 82 + 38, 8 + 3 * 82 + 35);   /* the System icon (2nd column, 4th row) */
    mouse_click();
    mouse_move(420, 330);
    save(th, "desktop_window");
    apps_open(APP_TERMINAL); run(300);
    mouse_move(46, 480 - 17);                   /* the NZEUS start button */
    mouse_click();
    mouse_move(600, 200);
    save(th, "desktop_menu");
    key(NZ_KEY_B);
    close_all_apps();
    for (int i = 0; i < 4; i++) {               /* close the info windows too */
        if (!desktop_window_count()) break;
        mouse_move(475, 55); mouse_click();
    }
    /* the keyboard in the start menu: Windows key, then DOWN twice */
    KbEvent super = { KB_SUPER, 0 }, down = { KB_DOWN, 0 }, esc = { KB_ESC, 0 };
    ui_kbd(&super); ui_kbd(&down); ui_kbd(&down); run(60);
    save(th, "desktop_menu_keys");
    ui_kbd(&esc); run(30);
    /* right-click on the empty desktop: the desktop menu */
    mouse_move(300, 250); ui_mouse_right_click(); run(60);
    save(th, "desktop_rclick");
    ui_kbd(&esc); run(30);
    /* a maximized window (Alt+Enter), then two windows side by side */
    apps_open(APP_FILES); run(200);
    KbEvent alt_enter = { KB_ALT_ENTER, 0 }, left = { KB_SUPER_LEFT, 0 }, right = { KB_SUPER_RIGHT, 0 };
    ui_kbd(&alt_enter); run(100);
    save(th, "desktop_max");
    ui_kbd(&alt_enter); ui_kbd(&left); run(60);
    apps_open(APP_TERMINAL); run(300);
    ui_kbd(&right); run(100);
    mouse_move(320, 470);
    save(th, "desktop_snap");
    close_all_apps();
    desktop_open_window(WIN_SHORTCUTS); run(100);
    save(th, "desktop_shortcuts");
    mouse_move(100 + 480 - 15, 14 + 15); mouse_click();   /* its X */
    ui_set_mode(UI_HANDHELD);
}

int main(int argc, char **argv)
{
    if (argc > 1) dir = argv[1];
    if (argc > 2) pyfile = argv[2];
    lv_init();
    nzeus_display_init();
    ui_init();
    desktop_sysinfo_fn = fake_sysinfo;
    wifi_sys_root = "/tmp/nz_preview_sys";      /* a pretend dongle (build script) */
    extern const char *python_examples_dir;
    python_examples_dir = "/tmp/nz_preview_pyex";
    extern int wifi_scan_wait_ms;
    wifi_scan_wait_ms = 200;
    ui_set_status("14:32", 78, 1);
    const char *only = getenv("NZ_THEME");      /* NZ_THEME=1: just one theme (quicker) */
    for (int th = 0; th < TH_COUNT; th++) {
        if (only && atoi(only) != th) continue;
        theme_set(th);
        render_all(th);
        printf("theme %d (%s): saved\n", th, theme()->name);
    }
    theme_set(TH_ORIGINAL);
    return 0;
}
