/* app_media.c - the NZEUS Media app: a music player (WAV, MP3) + a door to
 * the Images app.
 *
 *   A           play the song (or pause/continue the one playing)
 *   START       pause / continue          LEFT/RIGHT  10 seconds back / forward
 *   L1/R1       previous / next song      X           volume
 *   B           back (the music stops)
 * Songs: WAV and MP3 files in  music/  Music/  nzeus/music/  and the SD card's top folder. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "apps.h"
#include "audio.h"
#include "fsops.h"
#include "home.h"
#include "hw.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define MAX_SONGS 128
#define FIRST_SONG 1                      /* row 0 is "Pictures" */

static struct {
    lv_obj_t *title, *info, *bar, *time;
    NzList *list;
    char (*paths)[256];                   /* the songs found: borrowed while the app is open (32 KB) */
    int cap;                              /* how many fit (0 = no memory for the list) */
    int count, current;                   /* current: the song loaded (-1 none) */
    lv_timer_t *timer;
} M;
static char pending[256];

static int path_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && M.count < M.cap) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof M.paths[0]) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) scan_dir(p, depth - 1); continue; }
        if (fs_is_audio_file(e->d_name)) memcpy(M.paths[M.count++], p, strlen(p) + 1);
    }
    closedir(d);
}

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void fmt_time(char *b, size_t n, double s)
{
    int t = (int)(s + 0.5);
    snprintf(b, n, "%d:%02d", t / 60, t % 60);
}

/* the "now playing" card */
static void show_now(void)
{
    char a[16], b[16], t[48];
    if (M.current < 0 || !audio_loaded()) {
        lv_label_set_text(M.title, "Nothing playing");
        nz_button_text(M.info, M.count ? "Pick a song and press {accept}" : "Put WAV or MP3 files in the  music  folder");
        lv_bar_set_value(M.bar, 0, LV_ANIM_OFF);
        lv_label_set_text(M.time, "");
        return;
    }
    int rate, ch;
    audio_info(&rate, &ch);
    lv_label_set_text(M.title, base(M.paths[M.current]));
    lv_label_set_text_fmt(M.info, "%s   %s   %d Hz %s", audio_playing() ? LV_SYMBOL_PLAY " playing" : LV_SYMBOL_PAUSE " paused",
                          strstr(M.paths[M.current], ".mp3") || strstr(M.paths[M.current], ".MP3") ? "MP3" : "WAV",
                          rate, ch == 2 ? "stereo" : "mono");
    double pos = audio_position(), len = audio_length();
    lv_bar_set_value(M.bar, len > 0 ? (int)(1000 * pos / len) : 0, LV_ANIM_OFF);
    fmt_time(a, sizeof a, pos);
    fmt_time(b, sizeof b, len);
    snprintf(t, sizeof t, "%s / %s", a, b);
    lv_label_set_text(M.time, t);
}

static void play_song(int i)
{
    char err[96];
    if (i < 0 || i >= M.count) return;
    if (audio_open(M.paths[i], err, sizeof err) != 0) { ui_toast(err); M.current = -1; show_now(); return; }
    M.current = i;
    audio_play(1);
    nzlist_select(M.list, i + FIRST_SONG);
    show_now();
}

static void row_activated(int row, void *ctx)
{
    (void)ctx;
    if (row == 0) { apps_open(APP_IMAGES); return; }       /* "Pictures" */
    int i = row - FIRST_SONG;
    if (i == M.current && audio_loaded()) { audio_play(!audio_playing()); show_now(); }
    else play_song(i);
}

static void volume_chosen(int choice, void *ctx)
{
    (void)ctx;
    if (choice < 0) return;
    int v = 20 * (choice + 1);
    settings_set_int("volume", v);
    if (hw_set_volume(v) != 0) ui_toast("Volume: no sound device here");
}

static void tick_timer(lv_timer_t *t)
{
    (void)t;
    /* another app took the speaker (a WhatsApp voice message): our song is over,
     * and when that sound ends it must not start our next song */
    if (M.current >= 0 && audio_loaded() && strcmp(audio_path(), M.paths[M.current])) M.current = -1;
    if (audio_finished() && M.current >= 0) {             /* the song ended: the next one */
        if (M.current + 1 < M.count) play_song(M.current + 1);
        else { audio_close(); M.current = -1; }
    }
    show_now();
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&M, 0, sizeof M);
    M.current = -1;
    M.paths = calloc(MAX_SONGS, sizeof *M.paths);
    M.cap = M.paths ? MAX_SONGS : 0;
    if (!M.paths) ui_toast("Not enough memory to list the songs");
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = th_box(root, TS_PANEL);               /* now playing */
    lv_obj_set_size(card, LV_PCT(100), 96);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_MUSIC, 48));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, -6);
    M.title = th_label(card, "", TS_HEADING);
    lv_label_set_long_mode(M.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(M.title, LV_PCT(80));
    lv_obj_align(M.title, LV_ALIGN_TOP_LEFT, 76, 8);
    M.info = th_label(card, "", TS_SMALL);
    lv_obj_align(M.info, LV_ALIGN_TOP_LEFT, 76, 38);
    M.bar = lv_bar_create(card);
    lv_bar_set_range(M.bar, 0, 1000);
    lv_obj_set_size(M.bar, LV_PCT(66), 6);
    lv_obj_align(M.bar, LV_ALIGN_BOTTOM_LEFT, 76, -14);
    th_add(M.bar, TS_BTN);
    th_add_to(M.bar, TS_BTN_ACCENT, LV_PART_INDICATOR);
    M.time = th_label(card, "", TS_SMALL);
    lv_obj_align(M.time, LV_ALIGN_BOTTOM_RIGHT, -12, -8);

    M.list = nzlist_create(root, 0);
    lv_obj_set_width(nzlist_obj(M.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(M.list), 1);
    nzlist_on_activate(M.list, row_activated, NULL);
    nzlist_add(M.list, ICON_IMAGE, "Pictures", "opens the Images app", NULL);
    scan_dir(NZ_SDCARD, 0);
    scan_dir(NZ_SDCARD "/music", 2);
    scan_dir(NZ_SDCARD "/Music", 2);
    scan_dir(NZ_DIR "/music", 2);
    qsort(M.paths, M.count, sizeof M.paths[0], path_cmp);
    for (int i = 0; i < M.count; i++) {
        char folder[256], size[32] = "";
        snprintf(folder, sizeof folder, "%s", M.paths[i]);
        char *slash = strrchr(folder, '/');
        if (slash) *slash = 0;
        struct stat st;
        if (stat(M.paths[i], &st) == 0) fs_size_label((unsigned long long)st.st_size, size, sizeof size);
        const char *shown = fs_path_contains(NZ_SDCARD, folder) ? folder + strlen(NZ_SDCARD) : folder;
        nzlist_add(M.list, ICON_FILE_AUDIO, base(M.paths[i]), shown[0] ? shown : "/", size);
    }
    if (M.count) nzlist_select(M.list, FIRST_SONG);

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Play", "START", "Pause", "< >", "10 s", "L1/R1", "Song", "X", "Volume" };
    lv_obj_center(nz_hints(hint, hints, 5));

    if (pending[0]) {                                      /* opened on a file (Files app) */
        for (int i = 0; i < M.count; i++) if (!strcmp(M.paths[i], pending)) { play_song(i); break; }
        if (M.current < 0 && M.count < M.cap) {        /* a file outside the music folders */
            memcpy(M.paths[M.count], pending, sizeof M.paths[0]);
            nzlist_add(M.list, ICON_FILE_AUDIO, base(pending), "", NULL);
            play_song(M.count++);
        }
        pending[0] = 0;
    }
    show_now();
    M.timer = lv_timer_create(tick_timer, 250, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    switch (k) {
    case NZ_KEY_START: if (audio_loaded()) { audio_play(!audio_playing()); show_now(); } return 1;
    case NZ_KEY_LEFT:  if (audio_loaded()) { audio_seek(audio_position() - 10); show_now(); } return 1;
    case NZ_KEY_RIGHT: if (audio_loaded()) { audio_seek(audio_position() + 10); show_now(); } return 1;
    case NZ_KEY_L:     if (M.current > 0) play_song(M.current - 1); return 1;
    case NZ_KEY_R:     if (M.current + 1 < M.count) play_song(M.current + 1); return 1;
    case NZ_KEY_X: {
        static const char *const vols[] = { "20 %", "40 %", "60 %", "80 %", "100 %" };
        nz_menu("Volume", vols, 5, volume_chosen, NULL);
        return 1;
    }
    default: return nzlist_key(M.list, k);
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    audio_close();                                         /* closing Media stops the music */
    if (M.timer) lv_timer_del(M.timer);
    nzlist_free(M.list);
    free(M.paths);                                         /* the list's memory goes back */
    memset(&M, 0, sizeof M);
}

/* Play one file (the Files app calls this). */
int app_media_open(const char *path)
{
    snprintf(pending, sizeof pending, "%s", path);
    NzApp *a = apps_get(APP_MEDIA);
    if (a && a->root) apps_close(a);
    return apps_open(APP_MEDIA);
}

/* for tests */
int app_media_song_count(void) { return M.count; }

NzApp app_media = {
    .id = APP_MEDIA, .title = "Media", .icon = ICON_MEDIA,
    .create = create, .key = key, .destroy = destroy,
};
