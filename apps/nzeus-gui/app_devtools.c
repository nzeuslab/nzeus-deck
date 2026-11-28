/* app_devtools.c - the NZEUS Dev toolbox: the small everyday tools a coder
 * reaches for (number bases, Base64, hex, URL encoding, hashes, Unix time,
 * colours, JSON, regular expressions). All offline. The work itself is in
 * devtools.c; this file is only the screen.
 *
 *   UP/DOWN  pick a tool          A   type the input (the on-screen keyboard)
 *   X        encode <-> decode    Y   copy the answer (paste it in the Terminal/Editor)
 *   L1       paste as the input   B   back
 * Each tool starts with an example, so it shows what it does straight away. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "apps.h"
#include "devtools.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

#define IN_MAX  512
#define OUT_MAX 2048

enum { T_NUMBER, T_BASE64, T_HEX, T_URL, T_HASH, T_TIME, T_COLOUR, T_JSON, T_REGEX, T_COUNT };
static const struct { const char *name, *about, *example; int two_way; } tools[T_COUNT] = {
    { "Number bases", "Decimal, hex, binary and octal at once (0x.. 0b.. 0o.. or 'A')", "0xFF", 0 },
    { "Base64",       "Text <-> Base64 (used in e-mail, web pages and keys)",          "Hello NZEUS", 1 },
    { "Hex",          "Text <-> hex bytes, like a serial monitor shows them",           "Hi!", 1 },
    { "URL encoding", "Text <-> the %20 form used in web addresses",                    "a b&c=d", 1 },
    { "Hash",         "SHA-256 and CRC-32 fingerprints of the text",                    "hello", 0 },
    { "Unix time",    "Epoch seconds <-> a date (empty = now)",                         "", 0 },
    { "Colour",       "#RRGGBB, R,G,B or 565:XXXX (small LCD screens)",                 "#FF8000", 0 },
    { "JSON",         "Checks JSON and lays it out neatly (L1 pastes from the Editor)", "{\"name\":\"NZEUS\",\"apps\":19,\"ok\":true,\"tags\":[\"deck\",\"lab\"]}", 0 },
    { "Regex",        "Which parts of a text match a pattern ({accept}: pattern, then text)",  "[0-9]+", 0 },
};

static struct {
    int tool;
    int decode[T_COUNT];             /* two-way tools: 1 = decoding */
    char (*in)[IN_MAX];              /* each tool's input (4.6 KB, borrowed while the app is open) */
    char regex_text[IN_MAX];         /* the regex tool's second input */
    char out[OUT_MAX];
    int ok;                          /* the last answer worked (0 = it says what's wrong) */
    lv_obj_t *title, *about, *in_lbl, *out_lbl, *swatch, *hint_bar, *hint;
    lv_timer_t *timer;
} D;

/* ---------- working it out ---------- */

static void compute(void)
{
    const char *in = D.in[D.tool];
    int dec = D.decode[D.tool], rc = 0;
    uint32_t rgb = 0;
    switch (D.tool) {
    case T_NUMBER: rc = dt_number(in, D.out, sizeof D.out); break;
    case T_BASE64: rc = dec ? dt_b64_decode(in, D.out, sizeof D.out) : dt_b64_encode(in, D.out, sizeof D.out); break;
    case T_HEX:    rc = dec ? dt_hex_decode(in, D.out, sizeof D.out) : dt_hex_encode(in, D.out, sizeof D.out); break;
    case T_URL:    rc = dec ? dt_url_decode(in, D.out, sizeof D.out) : dt_url_encode(in, D.out, sizeof D.out); break;
    case T_HASH:   rc = dt_hash(in, D.out, sizeof D.out); break;
    case T_TIME:   rc = dt_time(in, time(NULL), D.out, sizeof D.out); break;
    case T_COLOUR: rc = dt_colour(in, D.out, sizeof D.out, &rgb); break;
    case T_JSON:   rc = dt_json(in, D.out, sizeof D.out); break;
    default:       rc = dt_regex(in, D.regex_text, D.out, sizeof D.out); break;
    }
    D.ok = rc == 0;
    if (D.tool == T_COLOUR && D.ok) {
        lv_obj_set_style_bg_color(D.swatch, lv_color_hex(rgb), 0);
        lv_obj_clear_flag(D.swatch, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(D.swatch, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- drawing ---------- */

static void set_hint(void)
{
    static const char *const h_two[] = { LV_SYMBOL_UP LV_SYMBOL_DOWN, "Tool", "A", "Type", "X", "Encode/decode", "Y", "Copy", "L1", "Paste", "B", "Back" };
    static const char *const h_one[] = { LV_SYMBOL_UP LV_SYMBOL_DOWN, "Tool", "A", "Type", "Y", "Copy", "L1", "Paste", "B", "Back" };
    if (D.hint) lv_obj_del(D.hint);
    D.hint = tools[D.tool].two_way ? nz_hints(D.hint_bar, h_two, 6) : nz_hints(D.hint_bar, h_one, 5);
    lv_obj_center(D.hint);
}

static void show(void)
{
    char s[IN_MAX * 2 + 64];
    if (!D.in) { lv_label_set_text(D.title, "Not enough memory - close other apps"); return; }
    compute();
    if (tools[D.tool].two_way)
        snprintf(s, sizeof s, "%s: %s   (%d of %d)", tools[D.tool].name, D.decode[D.tool] ? "decode" : "encode", D.tool + 1, T_COUNT);
    else snprintf(s, sizeof s, "%s   (%d of %d)", tools[D.tool].name, D.tool + 1, T_COUNT);
    lv_label_set_text(D.title, s);
    nz_button_text(D.about, tools[D.tool].about);
    if (D.tool == T_REGEX) snprintf(s, sizeof s, "Pattern:  %s\nText:  %s", D.in[T_REGEX], D.regex_text);
    else if (D.tool == T_TIME && !D.in[T_TIME][0]) snprintf(s, sizeof s, "Input:  (empty = now)");
    else snprintf(s, sizeof s, "Input:  %s", D.in[D.tool]);
    lv_label_set_text(D.in_lbl, s);
    lv_label_set_text(D.out_lbl, D.out);
    lv_obj_remove_style(D.out_lbl, th_style(TS_MONO), 0);
    lv_obj_remove_style(D.out_lbl, th_style(TS_MONO_ACCENT), 0);
    th_add(D.out_lbl, D.ok ? TS_MONO : TS_MONO_ACCENT);       /* a problem shows in the accent colour */
}

/* the clock in "Unix time" keeps ticking while its input is empty */
static void tick(lv_timer_t *t)
{
    (void)t;
    if (D.tool == T_TIME && !D.in[T_TIME][0]) show();
}

/* ---------- typing ---------- */

static void regex_text_typed(const char *text, void *ctx)
{
    (void)ctx;
    if (text) snprintf(D.regex_text, sizeof D.regex_text, "%s", text);
    show();
}

static void input_typed(const char *text, void *ctx)
{
    (void)ctx;
    if (!text || !D.in) return;                              /* cancelled (or the app closed): keep the old input */
    snprintf(D.in[D.tool], IN_MAX, "%s", text);
    show();
    if (D.tool == T_REGEX) nz_prompt("Text to search", D.regex_text, regex_text_typed, NULL);
}

/* ---------- the app ---------- */

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&D, 0, sizeof D);
    D.in = calloc(T_COUNT, sizeof *D.in);
    for (int i = 0; D.in && i < T_COUNT; i++) snprintf(D.in[i], IN_MAX, "%s", tools[i].example);
    snprintf(D.regex_text, sizeof D.regex_text, "R36S has 2 SD slots, 42 MB and 17 apps");
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    D.title = th_label(body, "", TS_HEADING);
    D.about = th_label(body, "", TS_MUTED);
    D.in_lbl = th_label(body, "", TS_TEXT);
    lv_obj_set_width(D.in_lbl, LV_PCT(100));
    lv_label_set_long_mode(D.in_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_height(D.in_lbl, 64, 0);            /* room for 2 lines (the regex tool: pattern + text) */

    lv_obj_t *panel = th_box(body, TS_PANEL);                 /* the answer */
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    D.out_lbl = th_label(panel, "", TS_MONO);
    lv_obj_set_width(D.out_lbl, LV_PCT(100));
    lv_label_set_long_mode(D.out_lbl, LV_LABEL_LONG_WRAP);
    D.swatch = lv_obj_create(panel);                          /* the colour tool's sample */
    lv_obj_remove_style_all(D.swatch);
    lv_obj_set_size(D.swatch, 96, 96);
    lv_obj_set_style_bg_opa(D.swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(D.swatch, 8, 0);
    lv_obj_align(D.swatch, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    D.hint_bar = th_box(root, TS_BAR);
    lv_obj_set_size(D.hint_bar, LV_PCT(100), 24);
    set_hint();
    show();
    D.timer = lv_timer_create(tick, 1000, NULL);
}

static void pick_tool(int t)
{
    D.tool = (t + T_COUNT) % T_COUNT;
    set_hint();
    show();
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (!D.in) return 0;                                     /* (no memory: only B, to leave) */
    switch (k) {
    case NZ_KEY_UP:   pick_tool(D.tool - 1); return 1;
    case NZ_KEY_DOWN: pick_tool(D.tool + 1); return 1;
    case NZ_KEY_A:
        nz_prompt(D.tool == T_REGEX ? "Pattern (like [0-9]+ or ^ab.c$)" : tools[D.tool].name, D.in[D.tool], input_typed, NULL);
        return 1;
    case NZ_KEY_X:
        if (!tools[D.tool].two_way) return 0;
        if (D.ok && D.out[0] && !strstr(D.out, "(not text")) {      /* turn the answer around (cut to the input's size) */
            size_t len = strlen(D.out) < IN_MAX - 1 ? strlen(D.out) : IN_MAX - 1;
            memcpy(D.in[D.tool], D.out, len);
            D.in[D.tool][len] = 0;
        }
        D.decode[D.tool] = !D.decode[D.tool];
        show();
        return 1;
    case NZ_KEY_Y:
        nz_clip_set(D.out);
        ui_toast("Copied - paste it in the Terminal or Editor");
        return 1;
    case NZ_KEY_L: {
        const char *c = nz_clip_get();
        if (!c[0]) { ui_toast("The clipboard is empty"); return 1; }
        snprintf(D.in[D.tool], IN_MAX, "%s", c);
        show();
        return 1;
    }
    default: return 0;                                       /* B: the app closes */
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    if (D.timer) lv_timer_del(D.timer);
    free(D.in);                                              /* the inputs' memory goes back */
    memset(&D, 0, sizeof D);
}

/* for tests */
const char *app_devtools_out(void) { return D.out; }
int app_devtools_tool(void) { return D.tool; }
const char *app_devtools_in(void) { return D.in ? D.in[D.tool] : ""; }

NzApp app_devtools = {
    .id = APP_DEVTOOLS, .title = "Dev toolbox", .icon = ICON_FILE_CODE,
    .create = create, .key = key, .destroy = destroy,
};
