/* shot.c - screenshots (PNG) and the command channel: request files in
 * /tmp/nzeus-cmd (written over SSH, e.g. by Claude on the PC): shot, press,
 * type, mouse, click, drag, open APP, home, and "web URL" (the Web app opens
 * NetSurf at that page, answered once NetSurf ends). See shot.h. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "apps.h"
#include "display.h"
#include "home.h"
#include "nzeus_tick.h"
#include "settings.h"
#include "shot.h"
#include "ui.h"
#include "usbkbd.h"

const char *shot_cmd_dir = "/tmp/nzeus-cmd";

/* the Web app (app_web.c): "web URL" opens a page and answers once it has loaded */
int app_web_open(const char *path);
int app_web_loading(void);
const char *app_web_result(void);
static char web_done[400];                    /* the answer file of a "web" request still loading */

/* ---------- a small PNG writer ----------
 * PNG = signature + chunks. The picture data is "deflate" compressed; we use
 * deflate's "stored" blocks (no compression at all), which every PNG reader
 * understands. A 640x480 screenshot is about 900 KB. */
static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const unsigned char *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (unsigned char)v; }

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char h[8];
    put32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(0xFFFFFFFFu, h + 4, 4);
    c = crc(c, data, len) ^ 0xFFFFFFFFu;
    put32(h, c);
    fwrite(h, 1, 4, f);
}

int shot_save_png(const char *path)
{
    const int w = NZ_W, h = NZ_H;
    size_t row = 1 + (size_t)w * 3, raw = row * h;          /* each row: filter byte 0 + RGB */
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;                  /* zlib header, stored blocks, Adler-32 */
    unsigned char *z = malloc(zlen);
    if (!z) return -1;
    size_t k = 0;
    z[k++] = 0x78; z[k++] = 0x01;                            /* zlib: deflate, no compression */
    uint32_t a = 1, b = 0;                                   /* Adler-32 of the raw data */
    size_t done = 0, x = 0;
    int y = 0;
    while (done < raw) {
        size_t n = raw - done > 65535 ? 65535 : raw - done;
        z[k++] = done + n == raw ? 1 : 0;                    /* last block? */
        z[k++] = n & 255; z[k++] = n >> 8;
        z[k++] = ~n & 255; z[k++] = (~n >> 8) & 255;
        for (size_t i = 0; i < n; i++) {                     /* the raw bytes, made on the fly */
            unsigned char v;
            if (x == 0) v = 0;
            else {
                uint16_t p = nzeus_picture[y * w + (int)((x - 1) / 3)];   /* (the screen itself when drawn there) */
                int c = (int)((x - 1) % 3);
                v = c == 0 ? (unsigned char)(((p >> 11) & 31) * 255 / 31)
                  : c == 1 ? (unsigned char)(((p >> 5) & 63) * 255 / 63)
                           : (unsigned char)((p & 31) * 255 / 31);
            }
            z[k++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            if (++x == row) { x = 0; y++; }
        }
        done += n;
    }
    put32(z + k, (b << 16) | a);
    k += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit RGB */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)k);
    chunk(f, "IEND", NULL, 0);
    int ok = fclose(f) == 0;
    free(z);
    return ok ? 0 : -1;
}

/* Screenshot menu item and SELECT + L1: nzeus/shots/shot-<date>.png
 * (two in the same second: shot-<date>-2.png ...). ui.c shows the message. */
int shot_take(char *name, size_t n)
{
    char dir[160], path[240], when[32];
    time_t now = time(NULL);
    snprintf(dir, sizeof dir, "%s/shots", NZ_DIR);
    mkdir(NZ_DIR, 0755);
    mkdir(dir, 0755);
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(name, n, "shot-%s.png", when);
    for (int k = 2; k < 100; k++) {                          /* don't overwrite one from this second */
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (access(path, F_OK) != 0) break;
        snprintf(name, n, "shot-%s-%d.png", when, k);
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (shot_save_png(path) != 0) return -1;
    sync();
    return 0;
}

/* ---------- the command channel ---------- */
static const struct { const char *name; int key; } buttons[] = {
    { "UP", NZ_KEY_UP }, { "DOWN", NZ_KEY_DOWN }, { "LEFT", NZ_KEY_LEFT }, { "RIGHT", NZ_KEY_RIGHT },
    { "A", NZ_KEY_A }, { "B", NZ_KEY_B }, { "X", NZ_KEY_X }, { "Y", NZ_KEY_Y },
    { "L1", NZ_KEY_L }, { "R1", NZ_KEY_R }, { "START", NZ_KEY_START }, { "SELECT", NZ_KEY_SELECT },
};

/* ---- the mouse (nzctl mouse / click / rclick / drag / wheel): through
 * ui_mouse_at(), the same way a real USB mouse moves the pointer ---- */
#define DRAG_STEPS 12                         /* frames from the start point to the end point */
static struct { int on, step, x1, y1, x2, y2; char done[400]; } drag;

/* exactly `count` whole numbers after the command word; 1 = ok */
static int numbers(const char *s, int *v, int count)
{
    int used = 0;
    for (int i = 0; i < count; i++) {
        int k = 0;
        if (sscanf(s + used, " %d%n", &v[i], &k) != 1) return 0;
        used += k;
    }
    while (s[used] == ' ') used++;
    return s[used] == 0;
}
static int on_screen(int x, int y) { return x >= 0 && x < NZ_W && y >= 0 && y < NZ_H; }

/* 1 = it was a mouse request (answer filled in, or "" while a drag runs) */
static int mouse_request(const char *line, char *answer, size_t n)
{
    static const struct { const char *word; int count; } kinds[] = {
        { "mouse ", 2 }, { "click ", 2 }, { "rclick ", 2 }, { "drag ", 4 }, { "wheel ", 1 } };
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        size_t len = strlen(kinds[k].word);
        if (strncmp(line, kinds[k].word, len)) continue;
        int v[4];
        if (!numbers(line + len, v, kinds[k].count)) {
            snprintf(answer, n, "error: %.*s needs %d whole number%s", (int)len - 1, kinds[k].word,
                     kinds[k].count, kinds[k].count > 1 ? "s" : "");
            return 1;
        }
        if (kinds[k].count >= 2 && (!on_screen(v[0], v[1]) || (kinds[k].count == 4 && !on_screen(v[2], v[3])))) {
            snprintf(answer, n, "error: outside the screen (x 0-%d, y 0-%d)", NZ_W - 1, NZ_H - 1);
            return 1;
        }
        int px, py;
        ui_pointer_pos(&px, &py);
        switch (k) {
        case 0: ui_mouse_at(v[0], v[1], 0, 0); snprintf(answer, n, "ok"); break;
        case 1: ui_mouse_at(v[0], v[1], 0, 1); snprintf(answer, n, "ok"); break;
        case 2: ui_mouse_at(v[0], v[1], 0, 0);  /* the pointer goes there, then the right button */
                ui_mouse_right_click();
                snprintf(answer, n, "ok"); break;
        case 3: if (drag.on) { snprintf(answer, n, "error: a drag is still running"); break; }
                drag.on = 1; drag.step = 0;
                drag.x1 = v[0]; drag.y1 = v[1]; drag.x2 = v[2]; drag.y2 = v[3];
                answer[0] = 0;                 /* answered when the drag ends (shot_poll) */
                break;
        default: ui_mouse_at(px, py, 0, 0);    /* (a mouse event: desktop mode, pointer shown) */
                ui_mouse_wheel(v[0]);           /* + = up: scrolls what is under the pointer */
                snprintf(answer, n, "ok"); break;
        }
        return 1;
    }
    return 0;
}

/* one frame of a running drag: press at the start, move, let go at the end */
static int drag_frame(void)
{
    if (!drag.on) return 0;
    int s = drag.step++;
    if (s <= DRAG_STEPS) {
        ui_mouse_at(drag.x1 + (drag.x2 - drag.x1) * s / DRAG_STEPS,
                    drag.y1 + (drag.y2 - drag.y1) * s / DRAG_STEPS, 1, 0);
        return 1;
    }
    ui_mouse_at(drag.x2, drag.y2, 0, 0);       /* let go */
    drag.on = 0;
    return 2;                                  /* finished: answer now */
}

/* "open car", "open Wi-Fi", "open task manager": by the app's name, any case,
 * spaces and dashes left out */
static void squeeze(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++)
        if (*in != ' ' && *in != '-') out[k++] = (char)(*in >= 'A' && *in <= 'Z' ? *in + 32 : *in);
    out[k] = 0;
}
static void open_request(const char *name, char *answer, size_t n)
{
    char want[40], have[40], list[300] = "";
    squeeze(name, want, sizeof want);
    for (int app = 0; app < APP_ALL_COUNT; app++) {
        squeeze(home_app_name(app), have, sizeof have);
        if (!strcmp(want, have)) {
            if (ui_open_app(app)) snprintf(answer, n, "ok");
            else snprintf(answer, n, "error: %s could not be opened", home_app_name(app));
            return;
        }
        snprintf(list + strlen(list), sizeof list - strlen(list), " %s", have);
    }
    snprintf(answer, n, "error: no app called '%.40s' - the apps are:%s", name, list);
}

static void run_request(const char *line, char *answer, size_t n)
{
    if (!strncmp(line, "shot ", 5)) {
        const char *path = line + 5;
        if (path[0] != '/') { snprintf(answer, n, "error: give a full path"); return; }
        if (shot_save_png(path) == 0) snprintf(answer, n, "ok");
        else snprintf(answer, n, "error: could not write %.200s", path);
        return;
    }
    if (!strncmp(line, "press ", 6)) {
        char copy[512];
        snprintf(copy, sizeof copy, "%s", line + 6);
        for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
            int found = -1;
            for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
                if (!strcmp(tok, buttons[i].name)) found = buttons[i].key;
            if (found < 0) { snprintf(answer, n, "error: unknown button %s", tok); return; }
            ui_key(found);
        }
        snprintf(answer, n, "ok");
        return;
    }
    if (!strncmp(line, "type ", 5)) {
        /* the two characters \n (backslash, n) = the Enter key: a request is one line, so
         * "nzctl type 'ls\n'" types ls and presses Enter (a real \\n stays a backslash + n) */
        for (const char *c = line + 5; *c; c++) {
            KbEvent ev = { KB_CHAR, *c };
            if (*c == '\n') { ev.key = KB_ENTER; ev.ch = 0; }
            else if (c[0] == '\\' && c[1] == 'n') { ev.key = KB_ENTER; ev.ch = 0; c++; }
            else if (c[0] == '\\' && c[1] == '\\') c++;          /* "\\" = one backslash */
            ui_kbd(&ev);
        }
        snprintf(answer, n, "ok");
        return;
    }
    if (mouse_request(line, answer, n)) return;
    if (!strncmp(line, "open ", 5)) { open_request(line + 5, answer, n); return; }
    if (!strncmp(line, "web ", 4)) {           /* "web https://example.com": answered when loaded (shot_poll) */
        if (web_done[0]) snprintf(answer, n, "error: a page is still loading");
        else if (!app_web_open(line + 4)) snprintf(answer, n, "error: the Web app could not be opened");
        else answer[0] = 0;
        return;
    }
    if (!strcmp(line, "home")) {               /* back to the home screen, like B on each open app */
        ui_set_mode(UI_HANDHELD);
        NzApp *a;
        for (int i = 0; i < 32 && (a = apps_focused()) && !a->in_window; i++) apps_close(a);
        ui_show_handheld_home();
        snprintf(answer, n, "ok");
        return;
    }
    snprintf(answer, n, "error: unknown request");
}

/* answer file: written whole, then renamed (the helper never reads half of it) */
static void write_answer(const char *done, const char *answer)
{
    char tmp[410];
    snprintf(tmp, sizeof tmp, "%s.tmp", done);
    FILE *f = fopen(tmp, "w");
    if (f) { fprintf(f, "%s\n", answer); fclose(f); rename(tmp, done); }
}

void shot_poll(void)
{
    static uint32_t last;
    if (drag_frame() == 2) write_answer(drag.done, "ok");   /* a drag moves one step every frame */
    if (web_done[0] && !app_web_loading()) {                /* a "web" page finished loading (or failed) */
        write_answer(web_done, app_web_result());
        web_done[0] = 0;
    }
    uint32_t now = nzeus_tick_ms();
    if (now - last < 100) return;                            /* 10 times a second is plenty */
    last = now;
    DIR *d = opendir(shot_cmd_dir);
    if (!d) { mkdir(shot_cmd_dir, 0700); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "req.", 4) || strstr(e->d_name, ".tmp")) continue;
        char req[400], done[400], line[512] = "", answer[300];
        snprintf(req, sizeof req, "%.100s/%.200s", shot_cmd_dir, e->d_name);
        snprintf(done, sizeof done, "%.100s/done.%.200s", shot_cmd_dir, e->d_name + 4);
        FILE *f = fopen(req, "r");
        if (!f) continue;
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        unlink(req);
        line[strcspn(line, "\r\n")] = 0;
        run_request(line, answer, sizeof answer);
        if (!answer[0] && !strncmp(line, "web ", 4)) snprintf(web_done, sizeof web_done, "%s", done);
        else if (!answer[0]) snprintf(drag.done, sizeof drag.done, "%s", done);   /* a drag: answered at its end */
        else write_answer(done, answer);
    }
    closedir(d);
}
