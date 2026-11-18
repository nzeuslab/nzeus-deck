/* theme.c - the NZEUS themes and the shared styles every screen uses.
 *
 * How a live theme switch works:
 *   1. every screen adds the shared styles (th_add) instead of its own colours
 *   2. theme_set() rewrites those styles with the new theme's tokens
 *   3. lv_obj_report_style_change(NULL) tells LVGL "styles changed": it
 *      refreshes and redraws every object that uses them, on every screen.
 * LVGL's own widgets (keyboard, text boxes, lists, windows) get the same
 * colours through a small LVGL theme (apply_cb) layered on LVGL's default one. */
#include <string.h>
#include "lvgl.h"
#include "theme.h"
#include "settings.h"
#include "display.h"

/* ---------- the themes ---------- */
static const NzTheme themes[TH_COUNT] = {
    [TH_ORIGINAL] = {
        .id = "original", .name = "Original", .dark = 1,
        .bg = 0x0E1016, .bar = 0x1A1D24, .surface = 0x1E222A, .surface_hi = 0x283040,
        .border = 0x2C3442, .accent = 0x3CC878, .on_accent = 0x0E1016, .text = 0xEBF0F5,
        .muted = 0x969FA8, .danger = 0xC0392B, .on_danger = 0xFFFFFF, .watermark = 0x161A22,
        .term_bg = 0x0B0D11, .term_fg = 0xD3D1C7,
        .ed_bg = 0x1E1E1E, .ed_line = 0x2A2D2E, .ed_gutter = 0x858585, .ed_cursor = 0xAEAFAD,
        /* VS Code Dark+ */
        .syntax = { 0xD4D4D4, 0xC586C0, 0x569CD6, 0xCE9178, 0x6A9955, 0xB5CEA8,
                    0xDCDCAA, 0x4EC9B0, 0x9CDCFE, 0xFFD700, 0xDA70D6, 0x179FFF },
        .radius = 12, .crt = 0,
        .font_small = &lv_font_montserrat_12, .font_body = &lv_font_montserrat_16,
        .font_heading = &lv_font_montserrat_20, .font_big = &lv_font_montserrat_40,
    },
    [TH_GREEN_CRT] = {
        .id = "green_crt", .name = "Green CRT", .dark = 1,
        .bg = 0x010A03, .bar = 0x03160A, .surface = 0x041B0C, .surface_hi = 0x0B3A1A,
        .border = 0x0F6A2C, .accent = 0x41FF6E, .on_accent = 0x011A08, .text = 0x41FF6E,
        .muted = 0x1EA845, .danger = 0x41FF6E, .on_danger = 0x011A08, .watermark = 0x06290F,
        .term_bg = 0x010A03, .term_fg = 0x41FF6E,
        .ed_bg = 0x010A03, .ed_line = 0x062610, .ed_gutter = 0x1A8A3A, .ed_cursor = 0x8CFFA6,
        /* shades of phosphor green: bright for keywords, dim for comments */
        .syntax = { 0x41FF6E, 0xB4FFC4, 0xB4FFC4, 0x2BD25A, 0x168C38, 0x7DFF9B,
                    0xD2FFDC, 0x96FFAF, 0x41FF6E, 0x7DFF9B, 0x41FF6E, 0x2BD25A },
        .radius = 0, .crt = 1,
        .font_small = &lv_font_nzeus_mono_16, .font_body = &lv_font_nzeus_mono_16,
        .font_heading = &lv_font_nzeus_mono_24, .font_big = &lv_font_nzeus_mono_24,
    },
    [TH_AMBER_CRT] = {
        .id = "amber_crt", .name = "Amber CRT", .dark = 1,
        .bg = 0x0C0700, .bar = 0x1C1000, .surface = 0x211300, .surface_hi = 0x452900,
        .border = 0x6E4300, .accent = 0xFFB52E, .on_accent = 0x1C1000, .text = 0xFFB52E,
        .muted = 0xB07818, .danger = 0xFFB52E, .on_danger = 0x1C1000, .watermark = 0x2E1B00,
        .term_bg = 0x0C0700, .term_fg = 0xFFB52E,
        .ed_bg = 0x0C0700, .ed_line = 0x2A1800, .ed_gutter = 0x99660F, .ed_cursor = 0xFFD58A,
        .syntax = { 0xFFB52E, 0xFFE0A3, 0xFFE0A3, 0xE69A1A, 0x9C6410, 0xFFC966,
                    0xFFF0CC, 0xFFD27F, 0xFFB52E, 0xFFC966, 0xFFB52E, 0xE69A1A },
        .radius = 0, .crt = 1,
        .font_small = &lv_font_nzeus_mono_16, .font_body = &lv_font_nzeus_mono_16,
        .font_heading = &lv_font_nzeus_mono_24, .font_big = &lv_font_nzeus_mono_24,
    },
    [TH_DAYLIGHT] = {
        .id = "daylight", .name = "Daylight", .dark = 0,
        .bg = 0xEEF1F5, .bar = 0xFFFFFF, .surface = 0xFFFFFF, .surface_hi = 0xDDF3E6,
        .border = 0xCDD3DB, .accent = 0x168F4E, .on_accent = 0xFFFFFF, .text = 0x1B1F24,
        .muted = 0x5C6673, .danger = 0xC62828, .on_danger = 0xFFFFFF, .watermark = 0xDFE4EA,
        .term_bg = 0xFBFBFC, .term_fg = 0x24292F,
        .ed_bg = 0xFFFFFF, .ed_line = 0xEEF3F8, .ed_gutter = 0x8C959F, .ed_cursor = 0x1B1F24,
        /* VS Code Light+ */
        .syntax = { 0x1B1F24, 0xAF00DB, 0x0000FF, 0xA31515, 0x008000, 0x098658,
                    0x795E26, 0x267F99, 0x001080, 0x0431FA, 0x319331, 0x7B3814 },
        .radius = 12, .crt = 0,
        .font_small = &lv_font_montserrat_12, .font_body = &lv_font_montserrat_16,
        .font_heading = &lv_font_montserrat_20, .font_big = &lv_font_montserrat_40,
    },
    [TH_MIDNIGHT] = {
        .id = "midnight", .name = "Midnight", .dark = 1,
        .bg = 0x0A1024, .bar = 0x101935, .surface = 0x142040, .surface_hi = 0x1F3263,
        .border = 0x2A3E75, .accent = 0x6CB6FF, .on_accent = 0x0A1024, .text = 0xE6EDFF,
        .muted = 0x8C9BC4, .danger = 0xE5534B, .on_danger = 0xFFFFFF, .watermark = 0x121C3C,
        .term_bg = 0x070C1C, .term_fg = 0xD6E2FF,
        .ed_bg = 0x0B1228, .ed_line = 0x16213F, .ed_gutter = 0x4A5580, .ed_cursor = 0xC0CAF5,
        /* Tokyo Night style */
        .syntax = { 0xC0CAF5, 0xBB9AF7, 0xBB9AF7, 0x9ECE6A, 0x565F89, 0xFF9E64,
                    0x7AA2F7, 0x2AC3DE, 0xC0CAF5, 0xE0AF68, 0xBB9AF7, 0x7DCFFF },
        .radius = 12, .crt = 0,
        .font_small = &lv_font_montserrat_12, .font_body = &lv_font_montserrat_16,
        .font_heading = &lv_font_montserrat_20, .font_big = &lv_font_montserrat_40,
    },
    /* Retro Blue: the feel of a classic early-2000s PC desktop (our own colours
     * and drawing): beige window bodies, light-blue bars, blue selection. In
     * desktop mode (retro = 1) desktop.c adds the blue taskbar, the green start
     * button and blue title bars. */
    [TH_RETRO_BLUE] = {
        .id = "retro_blue", .name = "Retro Blue", .dark = 0,
        .bg = 0xECE9D8, .bar = 0xD6E2F8, .surface = 0xFFFFFF, .surface_hi = 0xC4D6F6,
        .border = 0x7F9DB9, .accent = 0x1F5BCB, .on_accent = 0xFFFFFF, .text = 0x101010,
        .muted = 0x5A6272, .danger = 0xD9481F, .on_danger = 0xFFFFFF, .watermark = 0xDCD8C4,
        .term_bg = 0x000000, .term_fg = 0xCFCFCF,
        .ed_bg = 0xFFFFFF, .ed_line = 0xEEF3FC, .ed_gutter = 0x8C959F, .ed_cursor = 0x101010,
        .syntax = { 0x101010, 0xAF00DB, 0x0000FF, 0xA31515, 0x008000, 0x098658,
                    0x795E26, 0x267F99, 0x001080, 0x0431FA, 0x319331, 0x7B3814 },
        .radius = 8, .crt = 0, .retro = 1,
        .font_small = &lv_font_montserrat_12, .font_body = &lv_font_montserrat_16,
        .font_heading = &lv_font_montserrat_20, .font_big = &lv_font_montserrat_40,
    },
};

static int current = TH_ORIGINAL;
static lv_style_t styles[TS_COUNT];
static lv_style_t st_kb, st_kb_key, st_kb_ctrl, st_kb_pressed, st_scrollbar;
static lv_theme_t nz_lv_theme;           /* LVGL theme: styles LVGL's own widgets */
static void (*listeners[8])(void);
static int listener_count;

const NzTheme *theme(void) { return &themes[current]; }
const NzTheme *theme_get(int i) { return &themes[(i >= 0 && i < TH_COUNT) ? i : 0]; }
int theme_index(void) { return current; }
lv_color_t th_color(uint32_t rgb) { return lv_color_hex(rgb); }
lv_style_t *th_style(int s) { return &styles[s]; }

void theme_on_change(void (*fn)(void))
{
    for (int i = 0; i < listener_count; i++) if (listeners[i] == fn) return;   /* already listening */
    if (listener_count < 8) listeners[listener_count++] = fn;
}

/* ---------- filling the styles from the tokens ---------- */
static void fill_styles(void)
{
    const NzTheme *t = theme();
    int small_r = t->radius ? 6 : 0;                 /* smaller things: smaller corners */
    for (int i = 0; i < TS_COUNT; i++) { lv_style_reset(&styles[i]); lv_style_init(&styles[i]); }
    lv_style_t *s;

    s = &styles[TS_SCREEN];
    lv_style_set_bg_color(s, th_color(t->bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->text));
    lv_style_set_text_font(s, t->font_body);

    s = &styles[TS_BAR];
    lv_style_set_bg_color(s, th_color(t->bar));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    if (t->crt) {                                    /* CRT: a thin line under/over bars */
        lv_style_set_border_color(s, th_color(t->border));
        lv_style_set_border_width(s, 1);
        lv_style_set_border_side(s, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_TOP);
    } else if (!t->dark) {                           /* light: a hairline keeps bars apart */
        lv_style_set_border_color(s, th_color(t->border));
        lv_style_set_border_width(s, 1);
        lv_style_set_border_side(s, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_TOP);
    }

    s = &styles[TS_PANEL];
    lv_style_set_bg_color(s, th_color(t->surface));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, th_color(t->border));
    lv_style_set_border_width(s, 1);
    lv_style_set_radius(s, t->radius ? 10 : 0);
    lv_style_set_text_color(s, th_color(t->text));
    lv_style_set_text_font(s, t->font_body);
    if (!t->dark) {                                  /* light theme: soft shadow instead of a dark edge */
        lv_style_set_shadow_color(s, lv_color_hex(0x000000));
        lv_style_set_shadow_opa(s, LV_OPA_20);
        lv_style_set_shadow_width(s, 12);
        lv_style_set_shadow_ofs_y(s, 3);
    }

    s = &styles[TS_TILE];
    lv_style_set_bg_color(s, th_color(t->surface));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, t->radius);
    lv_style_set_border_width(s, 3);
    lv_style_set_border_color(s, th_color(t->crt || !t->dark ? t->border : t->surface));
    if (t->crt || !t->dark) lv_style_set_border_width(s, 1);

    s = &styles[TS_TILE_SEL];
    lv_style_set_bg_color(s, th_color(t->surface_hi));
    lv_style_set_border_color(s, th_color(t->accent));
    lv_style_set_border_width(s, 3);
    if (t->crt) {                                    /* phosphor glow around the selection */
        lv_style_set_shadow_color(s, th_color(t->accent));
        lv_style_set_shadow_width(s, 18);
        lv_style_set_shadow_spread(s, 1);
        lv_style_set_shadow_opa(s, LV_OPA_40);
    }

    s = &styles[TS_ROW_SEL];
    lv_style_set_bg_color(s, th_color(t->surface_hi));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, small_r);
    if (t->crt) {                                    /* CRT: a bright frame around the row */
        lv_style_set_border_color(s, th_color(t->accent));
        lv_style_set_border_width(s, 1);
    }

    s = &styles[TS_BTN];
    lv_style_set_bg_color(s, th_color(t->surface_hi));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->text));
    lv_style_set_text_font(s, t->font_body);
    lv_style_set_radius(s, small_r);
    lv_style_set_shadow_width(s, 0);
    if (t->crt) { lv_style_set_border_color(s, th_color(t->border)); lv_style_set_border_width(s, 1); }

    s = &styles[TS_BTN_ACCENT];
    lv_style_set_bg_color(s, th_color(t->accent));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->on_accent));
    lv_style_set_img_recolor(s, th_color(t->on_accent));
    lv_style_set_radius(s, small_r);
    lv_style_set_shadow_width(s, 0);

    s = &styles[TS_BTN_DANGER];
    lv_style_set_bg_color(s, th_color(t->danger));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->on_danger));
    lv_style_set_radius(s, small_r);
    lv_style_set_shadow_width(s, 0);

    s = &styles[TS_TEXT];
    lv_style_set_text_color(s, th_color(t->text));
    lv_style_set_text_font(s, t->font_body);

    s = &styles[TS_SMALL];
    lv_style_set_text_color(s, th_color(t->muted));
    lv_style_set_text_font(s, t->font_small);

    s = &styles[TS_MUTED];
    lv_style_set_text_color(s, th_color(t->muted));
    lv_style_set_text_font(s, t->font_body);

    s = &styles[TS_TITLE];
    lv_style_set_text_color(s, th_color(t->accent));
    lv_style_set_text_font(s, t->font_body);

    s = &styles[TS_HEADING];
    lv_style_set_text_color(s, th_color(t->text));
    lv_style_set_text_font(s, t->font_heading);

    s = &styles[TS_BIG];
    lv_style_set_text_color(s, th_color(t->accent));
    lv_style_set_text_font(s, t->font_big);

    s = &styles[TS_ON_ACCENT];
    lv_style_set_text_color(s, th_color(t->on_accent));
    lv_style_set_text_font(s, t->font_body);

    s = &styles[TS_ICON];
    lv_style_set_img_recolor(s, th_color(t->accent));
    lv_style_set_img_recolor_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->accent));   /* (for text/symbol icons) */

    s = &styles[TS_ICON_TEXT];
    lv_style_set_img_recolor(s, th_color(t->text));
    lv_style_set_img_recolor_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->text));

    s = &styles[TS_WATERMARK];
    lv_style_set_text_color(s, th_color(t->watermark));
    lv_style_set_text_font(s, t->font_big);
    lv_style_set_img_recolor(s, th_color(t->watermark));
    lv_style_set_img_recolor_opa(s, LV_OPA_COVER);

    s = &styles[TS_MONO];
    lv_style_set_text_color(s, th_color(t->term_fg));
    lv_style_set_text_font(s, &lv_font_nzeus_mono_16);

    s = &styles[TS_MONO_ACCENT];
    lv_style_set_text_color(s, th_color(t->accent));
    lv_style_set_text_font(s, &lv_font_nzeus_mono_16);

    s = &styles[TS_ARC];
    lv_style_set_arc_color(s, th_color(t->surface_hi));
    lv_style_set_arc_width(s, t->crt ? 6 : 10);
    lv_style_set_arc_rounded(s, t->crt ? 0 : 1);

    s = &styles[TS_ARC_IND];
    lv_style_set_arc_color(s, th_color(t->accent));
    lv_style_set_arc_width(s, t->crt ? 6 : 10);
    lv_style_set_arc_rounded(s, t->crt ? 0 : 1);

    s = &styles[TS_TERM];
    lv_style_set_bg_color(s, th_color(t->term_bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);

    s = &styles[TS_INPUT];
    lv_style_set_bg_color(s, th_color(t->dark ? t->bar : t->bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->text));
    lv_style_set_border_color(s, th_color(t->border));
    lv_style_set_border_width(s, 1);
    lv_style_set_radius(s, small_r);

    s = &styles[TS_KEYCAP];
    lv_style_set_bg_color(s, th_color(t->crt ? t->accent : t->surface_hi));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, th_color(t->crt ? t->on_accent : t->text));
    lv_style_set_text_font(s, t->font_small);
    lv_style_set_radius(s, t->radius ? 4 : 0);
    lv_style_set_pad_hor(s, 6);
    lv_style_set_pad_ver(s, 1);

    /* LVGL's on-screen keyboard: keys look like our buttons */
    lv_style_reset(&st_kb);
    lv_style_init(&st_kb);
    lv_style_set_bg_color(&st_kb, th_color(t->bar));
    lv_style_set_bg_opa(&st_kb, LV_OPA_COVER);
    lv_style_set_border_width(&st_kb, 0);
    lv_style_set_radius(&st_kb, 0);
    lv_style_set_pad_all(&st_kb, 4);
    lv_style_set_pad_gap(&st_kb, 4);
    lv_style_reset(&st_kb_key);
    lv_style_init(&st_kb_key);
    lv_style_set_bg_color(&st_kb_key, th_color(t->surface));
    lv_style_set_bg_opa(&st_kb_key, LV_OPA_COVER);
    lv_style_set_text_color(&st_kb_key, th_color(t->text));
    lv_style_set_text_font(&st_kb_key, t->font_body);
    lv_style_set_radius(&st_kb_key, small_r);
    lv_style_set_shadow_width(&st_kb_key, 0);
    lv_style_set_border_width(&st_kb_key, t->crt || !t->dark ? 1 : 0);
    lv_style_set_border_color(&st_kb_key, th_color(t->border));
    lv_style_reset(&st_kb_ctrl);
    lv_style_init(&st_kb_ctrl);
    lv_style_set_bg_color(&st_kb_ctrl, th_color(t->surface_hi));
    lv_style_reset(&st_kb_pressed);
    lv_style_init(&st_kb_pressed);
    lv_style_set_bg_color(&st_kb_pressed, th_color(t->accent));
    lv_style_set_text_color(&st_kb_pressed, th_color(t->on_accent));
    lv_style_reset(&st_scrollbar);
    lv_style_init(&st_scrollbar);
    lv_style_set_bg_color(&st_scrollbar, th_color(t->border));
}

/* LVGL calls this for every new LVGL widget: give it NZEUS colours. */
static void apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_check_type(obj, &lv_keyboard_class)) {
        lv_obj_add_style(obj, &st_kb, 0);
        lv_obj_add_style(obj, &st_kb_key, LV_PART_ITEMS);
        lv_obj_add_style(obj, &st_kb_ctrl, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &st_kb_pressed, LV_PART_ITEMS | LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &styles[TS_INPUT], 0);
    } else if (lv_obj_check_type(obj, &lv_btn_class)) {
        lv_obj_add_style(obj, &styles[TS_BTN], 0);
    }   /* (LVGL's list widget is switched off in lv_conf.h: our lists are nzlist.c) */
    lv_obj_add_style(obj, &st_scrollbar, LV_PART_SCROLLBAR);
}

static void apply_lvgl_theme(void)
{
    const NzTheme *t = theme();
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *base = lv_theme_default_init(disp, th_color(t->accent), th_color(t->accent),
                                             t->dark, t->font_body);
    static int set_up;
    if (!set_up) {                                   /* our theme on top of LVGL's default */
        nz_lv_theme = *base;
        lv_theme_set_parent(&nz_lv_theme, base);
        lv_theme_set_apply_cb(&nz_lv_theme, apply_cb);
        lv_disp_set_theme(disp, &nz_lv_theme);
        set_up = 1;
    }
    nz_lv_theme.font_small = t->font_small;
    nz_lv_theme.font_normal = t->font_body;
    nz_lv_theme.font_large = t->font_heading;
    nz_lv_theme.color_primary = th_color(t->accent);
    nz_lv_theme.color_secondary = th_color(t->accent);
}

static void apply(void)
{
    fill_styles();
    apply_lvgl_theme();
    nzeus_display_set_crt(theme()->crt);
    lv_obj_report_style_change(NULL);                /* LVGL: every object, look again */
    for (int i = 0; i < listener_count; i++) listeners[i]();
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
}

void theme_init(void)
{
    const char *id = settings_get("theme", "original");
    current = TH_ORIGINAL;
    for (int i = 0; i < TH_COUNT; i++) if (!strcmp(themes[i].id, id)) current = i;
    for (int i = 0; i < TS_COUNT; i++) lv_style_init(&styles[i]);
    apply();
}

void theme_set(int index)
{
    if (index < 0 || index >= TH_COUNT) return;
    current = index;
    settings_set("theme", themes[index].id);
    apply();
}

/* ---------- helpers ---------- */
void th_add(lv_obj_t *o, int s) { lv_obj_add_style(o, &styles[s], 0); }
void th_add_to(lv_obj_t *o, int s, lv_style_selector_t sel) { lv_obj_add_style(o, &styles[s], sel); }

lv_obj_t *th_label(lv_obj_t *parent, const char *text, int s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_add_style(l, &styles[s], 0);
    return l;
}

lv_obj_t *th_box(lv_obj_t *parent, int s)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    if (s >= 0) lv_obj_add_style(o, &styles[s], 0);
    return o;
}
