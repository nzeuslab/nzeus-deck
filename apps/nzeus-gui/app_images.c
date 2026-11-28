/* app_images.c - the NZEUS Images app: a gallery of the pictures on the SD
 * card, and a viewer (PNG, JPG, BMP; see picture.c).
 *
 * Gallery:  UP/DOWN pick     A show                 B back
 * Viewer:   LEFT/RIGHT (or L1/R1) previous / next picture
 *           A zoom (1x, 2x, 4x)   D-pad while zoomed: move around   B back */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "apps.h"
#include "fsops.h"
#include "home.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "picture.h"
#include "pdfreader.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define MAX_PICS 128

static struct {
    lv_obj_t *root, *gallery_box, *view_box, *img, *caption, *msg;
    NzList *list;
    char (*paths)[256];            /* the pictures found: borrowed while the app is open (32 KB) */
    int cap;                       /* how many fit (0 = no memory for the list) */
    int count, current, viewing, zoom, pan_x, pan_y, from_gallery;
    Picture pic;
    PdfReader pdf;
    int is_pdf, pdf_zoom, pdf_rotation;
    lv_img_dsc_t dsc;
} I;
static char pending[256];                 /* a file to show as soon as the app opens */
static char pending_pdf[FS_PATH_MAX];

/* ---------- finding pictures ---------- */
static int path_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && I.count < I.cap) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof I.paths[0]) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) scan_dir(p, depth - 1); continue; }
        if (fs_is_image_file(e->d_name)) memcpy(I.paths[I.count++], p, strlen(p) + 1);   /* (length checked above) */
    }
    closedir(d);
}

/* the gallery: the usual picture folders */
static void scan_gallery(void)
{
    I.count = 0;
    scan_dir(NZ_SDCARD, 0);
    scan_dir(NZ_SDCARD "/pictures", 1);
    scan_dir(NZ_SDCARD "/Pictures", 1);
    scan_dir(NZ_SDCARD "/DCIM", 2);
    scan_dir(NZ_DIR "/shots", 0);                   /* screenshots (SELECT + L1, Screenshot) */
    scan_dir(NZ_DIR "/screenshots", 0);             /* (where older screenshots went) */
    qsort(I.paths, I.count, sizeof I.paths[0], path_cmp);
}

/* the viewer opened on one file: its neighbours are the pictures in its folder */
static void scan_folder_of(const char *file)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s", file);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    I.count = 0;
    scan_dir(dir, 0);
    qsort(I.paths, I.count, sizeof I.paths[0], path_cmp);
    I.current = 0;
    for (int i = 0; i < I.count; i++) if (!strcmp(I.paths[i], file)) I.current = i;
}

/* ---------- the viewer ---------- */
static void place_image(void)
{
    if (I.is_pdf && I.pic.px) {
        int bw = lv_obj_get_content_width(I.view_box), bh = lv_obj_get_content_height(I.view_box);
        int w = I.pdf_rotation % 2 ? I.pic.h : I.pic.w;
        int h = I.pdf_rotation % 2 ? I.pic.w : I.pic.h;
        int scale = bw * 256 / w;
        if (!I.pdf_zoom && scale > bh * 256 / h) scale = bh * 256 / h;
        if (I.pdf_zoom > 1) scale *= 2;
        if (scale < 1) scale = 1;
        int mx = (w * scale / 256 - bw) / 2, my = (h * scale / 256 - bh) / 2;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (I.pan_x > mx) I.pan_x = mx;
        if (I.pan_x < -mx) I.pan_x = -mx;
        if (I.pan_y > my) I.pan_y = my;
        if (I.pan_y < -my) I.pan_y = -my;
        lv_img_set_zoom(I.img, (uint16_t)scale);
        lv_img_set_angle(I.img, (int16_t)(I.pdf_rotation * 900));
        lv_obj_align(I.img, LV_ALIGN_CENTER, I.pan_x, I.pan_y);
        return;
    }
    lv_img_set_zoom(I.img, (uint16_t)(256 * I.zoom));
    lv_obj_align(I.img, LV_ALIGN_CENTER, I.pan_x, I.pan_y);
}

static void pdf_loading(void)
{
    lv_img_set_src(I.img, NULL);
    lv_obj_add_flag(I.img, LV_OBJ_FLAG_HIDDEN);
    picture_free(&I.pic);
    I.pdf_zoom = 0; I.pan_x = I.pan_y = 0;
    nz_button_text(I.msg, "Opening PDF page...\n{back}: cancel");
    lv_label_set_text_fmt(I.caption, "Page %d / %d", I.pdf.page, I.pdf.pages);
}

static void tick(NzApp *a)
{
    (void)a;
    if (!I.is_pdf) return;
    int ready = pdfreader_poll(&I.pdf);
    if (!ready) return;
    if (ready < 0) { lv_label_set_text(I.msg, I.pdf.error); return; }
    char error[96];
    if (picture_load_pdf(I.pdf.bitmap, &I.pic, error, sizeof error)) {
        lv_label_set_text(I.msg, error); return;
    }
    memset(&I.dsc, 0, sizeof I.dsc);
    I.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    I.dsc.header.w = (uint32_t)I.pic.w; I.dsc.header.h = (uint32_t)I.pic.h;
    I.dsc.data_size = (uint32_t)(I.pic.w * I.pic.h * 2);
    I.dsc.data = (const uint8_t *)I.pic.px;
    lv_img_cache_invalidate_src(&I.dsc);
    lv_img_set_src(I.img, &I.dsc);
    lv_obj_clear_flag(I.img, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(I.msg, "");
    lv_label_set_text_fmt(I.caption, "Page %d / %d", I.pdf.page, I.pdf.pages);
    place_image();
}

static void show_current(void)
{
    char err[96], cap[200];
    lv_obj_add_flag(I.img, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(I.img, NULL);
    picture_free(&I.pic);
    I.zoom = 1;
    I.pan_x = I.pan_y = 0;
    if (I.count == 0) { lv_label_set_text(I.msg, "No pictures here."); return; }
    const char *path = I.paths[I.current], *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    lv_obj_update_layout(I.view_box);
    int bw = lv_obj_get_content_width(I.view_box), bh = lv_obj_get_content_height(I.view_box);
    if (picture_load(path, bw > 0 ? bw : 640, bh > 0 ? bh : 420, &I.pic, err, sizeof err) != 0) {
        lv_label_set_text_fmt(I.msg, "%s\n\n%s", name, err);
        snprintf(cap, sizeof cap, "%s     %d / %d", name, I.current + 1, I.count);
        lv_label_set_text(I.caption, cap);
        return;
    }
    memset(&I.dsc, 0, sizeof I.dsc);
    I.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    I.dsc.header.w = (uint32_t)I.pic.w;
    I.dsc.header.h = (uint32_t)I.pic.h;
    I.dsc.data_size = (uint32_t)(I.pic.w * I.pic.h * 2);
    I.dsc.data = (const uint8_t *)I.pic.px;
    lv_img_cache_invalidate_src(&I.dsc);
    lv_img_set_src(I.img, &I.dsc);
    lv_obj_clear_flag(I.img, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(I.msg, "");
    place_image();
    snprintf(cap, sizeof cap, "%s     %d x %d     %d / %d", name, I.pic.orig_w, I.pic.orig_h, I.current + 1, I.count);
    lv_label_set_text(I.caption, cap);
}

static void set_viewing(int on)
{
    I.viewing = on;
    if (on) { lv_obj_add_flag(I.gallery_box, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(I.view_box, LV_OBJ_FLAG_HIDDEN); }
    else {
        lv_obj_clear_flag(I.gallery_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(I.view_box, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(I.img, NULL);
        picture_free(&I.pic);                          /* give the memory back while browsing */
    }
}

static void view_index(int i, void *ctx)
{
    (void)ctx;
    I.current = i;
    I.from_gallery = 1;
    set_viewing(1);
    show_current();
}

static void step(int dir)
{
    if (I.count == 0) return;
    I.current = (I.current + dir + I.count) % I.count;
    show_current();
}

static void fill_gallery(void)
{
    nzlist_clear(I.list);
    scan_gallery();
    for (int i = 0; i < I.count; i++) {
        const char *name = strrchr(I.paths[i], '/');
        char folder[256], size[32] = "";
        snprintf(folder, sizeof folder, "%s", I.paths[i]);
        char *slash = strrchr(folder, '/');
        if (slash) *slash = 0;
        struct stat st;
        if (stat(I.paths[i], &st) == 0) fs_size_label((unsigned long long)st.st_size, size, sizeof size);
        const char *shown = fs_path_contains(NZ_SDCARD, folder) ? folder + strlen(NZ_SDCARD) : folder;
        nzlist_add(I.list, ICON_FILE_IMAGE, name ? name + 1 : I.paths[i], shown[0] ? shown : "/", size);
    }
    if (I.count == 0) nzlist_add(I.list, ICON_IMAGE, "No pictures yet", "Put PNG, JPG or BMP files in the  pictures  folder", NULL);
}

/* ---------- the app ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&I, 0, sizeof I);
    I.root = root;
    I.is_pdf = pending_pdf[0] != 0;
    apps_set_title(a, I.is_pdf ? "PDF Reader" : "Images");
    I.paths = calloc(MAX_PICS, sizeof *I.paths);
    I.cap = I.paths ? MAX_PICS : 0;
    if (!I.paths) ui_toast("Not enough memory to list the pictures");
    th_add(root, TS_SCREEN);

    I.gallery_box = th_box(root, -1);                /* the gallery */
    lv_obj_set_size(I.gallery_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(I.gallery_box, LV_FLEX_FLOW_COLUMN);
    I.list = nzlist_create(I.gallery_box, 0);
    lv_obj_set_width(nzlist_obj(I.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(I.list), 1);
    nzlist_on_activate(I.list, view_index, NULL);
    lv_obj_t *gh = th_box(I.gallery_box, TS_BAR);
    lv_obj_set_size(gh, LV_PCT(100), 26);
    static const char *const ghints[] = { "A", "Show", "B", "Back" };
    lv_obj_center(nz_hints(gh, ghints, 2));

    I.view_box = th_box(root, -1);                   /* the viewer */
    lv_obj_set_size(I.view_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(I.view_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(I.view_box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_bottom(I.view_box, 26, 0);  /* room for the caption */
    I.img = lv_img_create(I.view_box);
    lv_img_set_antialias(I.img, I.is_pdf);           /* smooth page scaling; photos keep their old behavior */
    I.msg = th_label(I.view_box, "", TS_MUTED);
    lv_obj_set_style_text_align(I.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(I.msg);
    lv_obj_t *cap = th_box(I.view_box, TS_BAR);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(cap, LV_PCT(100), 26);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 26);
    I.caption = th_label(cap, "", TS_SMALL);
    lv_obj_align(I.caption, LV_ALIGN_LEFT_MID, 10, 0);
    static const char *const vhints[] = { "< >", "Next", "A", "Zoom", "B", "Back" };
    static const char *const phints[] = { "L/R", "Page", "A", "Zoom", "D-pad", "Scroll", "X", "Rotate", "B", "Back" };
    lv_obj_align(nz_hints(cap, I.is_pdf ? phints : vhints, I.is_pdf ? 5 : 3), LV_ALIGN_RIGHT_MID, -8, 0);

    if (I.is_pdf) {
        set_viewing(1); pdf_loading();
        if (pdfreader_open(&I.pdf, pending_pdf)) lv_label_set_text(I.msg, I.pdf.error);
        pending_pdf[0] = 0;
    } else if (pending[0]) {                         /* opened on a file (Files app) */
        scan_folder_of(pending);
        pending[0] = 0;
        I.from_gallery = 0;
        set_viewing(1);
        show_current();
    } else {
        fill_gallery();
        set_viewing(0);
    }
}

static int key(NzApp *a, int k)
{
    if (I.is_pdf) {
        if (k == NZ_KEY_B) return 0;
        if (k == NZ_KEY_L || k == NZ_KEY_R) {
            int page = I.pdf.page + (k == NZ_KEY_L ? -1 : 1);
            if (I.pdf.pages && page >= 1 && page <= I.pdf.pages && !pdfreader_page(&I.pdf, page)) pdf_loading();
            return 1;
        }
        if (!I.pic.px) return 1;
        if (k == NZ_KEY_X) {
            I.pdf_rotation = (I.pdf_rotation + 1) % 4;
            I.pdf_zoom = 0; I.pan_x = I.pan_y = 0;
        } else if (k == NZ_KEY_A) {
            I.pdf_zoom = (I.pdf_zoom + 1) % 3;
            I.pan_x = 0; I.pan_y = I.pdf_zoom ? I.pic.h * 2 : 0; /* start zoom at the top */
        } else if (k == NZ_KEY_LEFT) I.pan_x += 60;
        else if (k == NZ_KEY_RIGHT) I.pan_x -= 60;
        else if (k == NZ_KEY_UP) I.pan_y += 60;
        else if (k == NZ_KEY_DOWN) I.pan_y -= 60;
        else return 0;
        place_image();
        return 1;
    }
    if (!I.viewing) {
        if (k == NZ_KEY_B) return 0;                 /* leave the app */
        return nzlist_key(I.list, k);
    }
    int zoomed = I.zoom > 1, move = 40 * I.zoom;
    switch (k) {
    case NZ_KEY_A:     I.zoom = I.zoom >= 4 ? 1 : I.zoom * 2; if (I.zoom == 1) I.pan_x = I.pan_y = 0; place_image(); return 1;
    case NZ_KEY_L:     step(-1); return 1;
    case NZ_KEY_R:     step(+1); return 1;
    case NZ_KEY_LEFT:  if (zoomed) { I.pan_x += move; place_image(); } else step(-1); return 1;
    case NZ_KEY_RIGHT: if (zoomed) { I.pan_x -= move; place_image(); } else step(+1); return 1;
    case NZ_KEY_UP:    if (zoomed) { I.pan_y += move; place_image(); } return 1;
    case NZ_KEY_DOWN:  if (zoomed) { I.pan_y -= move; place_image(); } return 1;
    case NZ_KEY_B:
        if (I.from_gallery) { set_viewing(0); return 1; }   /* back to the gallery */
        (void)a;
        return 0;                                    /* opened from Files: leave */
    default: return 0;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    lv_img_set_src(I.img, NULL);
    picture_free(&I.pic);
    pdfreader_close(&I.pdf);
    nzlist_free(I.list);
    free(I.paths);                                   /* the list's memory goes back */
    memset(&I, 0, sizeof I);
    a->title = "Images";
}

/* Show one picture (the Files app calls this). */
int app_images_open(const char *path)
{
    pending_pdf[0] = 0;
    snprintf(pending, sizeof pending, "%s", path);
    NzApp *a = apps_get(APP_IMAGES);
    if (a && a->root) apps_close(a);                 /* already open: start again on this file */
    return apps_open(APP_IMAGES);
}

int app_images_open_pdf(const char *path)
{
    if (!path || strlen(path) >= sizeof pending_pdf) { ui_toast("PDF path is too long"); return 0; }
    snprintf(pending_pdf, sizeof pending_pdf, "%s", path);
    pending[0] = 0;
    NzApp *a = apps_get(APP_IMAGES);
    if (a && a->root) apps_close(a);
    if (a) a->title = "PDF Reader";
    return apps_open(APP_IMAGES);
}

/* for tests */
int app_images_shown_w(void) { return I.viewing && I.pic.px ? I.pic.w : 0; }
int app_images_count(void) { return I.count; }
int app_images_pdf_page(void) { return I.is_pdf ? I.pdf.page : 0; }
int app_images_pdf_pages(void) { return I.is_pdf ? I.pdf.pages : 0; }
int app_images_pdf_zoom(void) { return I.pdf_zoom; }
int app_images_pdf_pan_y(void) { return I.pan_y; }
const char *app_images_pdf_error(void) { return I.pdf.error; }

NzApp app_images = {
    .id = APP_IMAGES, .title = "Images", .icon = ICON_IMAGE,
    .create = create, .key = key, .tick = tick, .destroy = destroy,
};
