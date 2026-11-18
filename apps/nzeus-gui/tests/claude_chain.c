/* claude_chain.c - the NZEUS Claude app (PC build) on the real SSH chain.
 *
 * Run by brain/test_chain.py against an isolated loopback SSH fixture.
 * Exercises the real app, PIN vault, modern helper, SSH client and PTY.
 * The remote Claude response is a fixture: no account or real brain is used.
 *
 * Usage: claude_chain CONNECT_SCRIPT HOST PORT USER HOLD_SECONDS PICTURE.ppm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "display.h"
#include "home.h"
#include "nzeus_tick.h"
#include "nzui.h"
#include "settings.h"
#include "ui.h"
#include "usbkbd.h"
#include "vtwidget.h"

extern const char *claude_connect_script;      /* app_claude.c */
void claude_pin_set(const char *pin);
lv_obj_t *app_claude_terminal(void);
int shot_save_png(const char *path);

static int failures;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) failures++; fflush(stdout); }
static void run(int ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)ms;
    while ((int32_t)(end - nzeus_tick_ms()) > 0) { ui_tick(); lv_timer_handler(); usleep(5000); }
}
static void type(const char *s) { for (; *s; s++) { KbEvent e = { KB_CHAR, *s }; ui_kbd(&e); } KbEvent en = { KB_ENTER, 0 }; ui_kbd(&en); }

static int screen_has(const char *text)
{
    lv_obj_t *vt = app_claude_terminal();
    char row[256];
    for (int r = 0; vt && r < 30; r++) if (strstr(vtw_row_text(vt, r, row, sizeof row), text)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: claude_chain SCRIPT HOST PORT USER HOLD PICTURE.png\n"); return 2; }
    unsetenv("NZ_FORCE_PIPES");
    if (system("mkdir -p /tmp/nzchain_sd/nzeus") != 0) return 2;   /* isolated test paths; runner owns cleanup */
    lv_init();
    nzeus_display_init();
    ui_init();
    claude_connect_script = argv[1];
    settings_set("claude_pc_host", argv[2]);
    settings_set("claude_pc_port", argv[3]);
    settings_set("claude_pc_user", argv[4]);
    claude_pin_set("2468");

    ui_open_app(APP_CLAUDE); run(100);              /* "Connect" is selected */
    ui_key(NZ_KEY_A); run(50);
    type("0000"); run(100);
    check(app_claude_terminal() == NULL && access("/tmp/nzeus-run/console_key", F_OK) != 0,
          "wrong PIN neither connects nor leaves an unlocked key");
    ui_key(NZ_KEY_A); run(50); type("2468");
    int ok = 0;
    for (int i = 0; i < 300 && !ok; i++) { run(50); ok = screen_has("FAKE-CLAUDE-BANNER"); }
    check(ok, "NZEUS Claude app: real modern helper/SSH reaches the brain fixture and draws its banner");
    char want[32];
    lv_obj_t *vt = app_claude_terminal();
    int rows = vt ? lv_obj_get_height(vt) / VTW_CELL_H : 0, cols = vt ? lv_obj_get_width(vt) / VTW_CELL_W : 0;
    snprintf(want, sizeof want, "size: %d %d", rows, cols);
    if (!screen_has(want)) {
        char line[256];
        printf("SIZE expected %s, LINES=%s COLUMNS=%s\n", want, getenv("LINES"), getenv("COLUMNS"));
        for (int r = 0; vt && r < 5; r++) printf("ROW %s\n", vtw_row_text(vt, r, line, sizeof line));
    }
    check(screen_has(want), "NZEUS Claude app: the brain's terminal has the console's size");
    check(vt && vtw_uses_pty(vt), "modern Claude connection uses a real local PTY");
    type("chain-roundtrip");
    ok = 0;
    for (int i = 0; i < 100 && !ok; i++) { run(50); ok = screen_has("ACK:chain-roundtrip"); }
    check(ok, "typed text crosses the real SSH channel and its reply reaches the screen");
    shot_save_png(argv[6]);
    printf("READY\n");                               /* test screen is ready */
    fflush(stdout);
    run(atoi(argv[5]) * 1000);
    check(app_claude_terminal() != NULL, "NZEUS Claude app: still connected after interactive round trip");
    ui_key(NZ_KEY_START); run(50); ui_key(NZ_KEY_A); run(300);   /* START > Disconnect */
    check(app_claude_terminal() == NULL, "NZEUS Claude app: Disconnect ends it");
    check(access("/tmp/nzeus-run/console_key", F_OK) != 0 &&
          access("/tmp/nzchain_sd/nzeus/claude/console_key.enc", F_OK) == 0 &&
          access("/tmp/nzchain_sd/nzeus/claude/console_key", F_OK) != 0,
          "disconnect removes the RAM key and retains only the encrypted card key");
    return failures ? 1 : 0;
}
