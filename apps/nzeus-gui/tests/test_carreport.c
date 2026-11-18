/* test_carreport.c - checks for the car report PDF maker (carreport*.c).
 *   test_carreport <out folder>        all checks + makes the preview PDFs
 * Every check prints "PASS ..." or "FAIL ..."; the exit code is the number of
 * failed checks. The PDFs land in <out folder>:
 *   sample/  the 29 Sep 2026 Raptor scan, English + Arabic (to compare with the originals)
 *   long/    60 fault codes (page breaks, repeated table header)
 *   demo/    the same scan marked as DEMO DATA
 *   sick/    active + pending codes (red summary box)
 *   odd/     empty and garbage scans (must not crash)
 * Built and run by R36S/build_carreport_test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "carscan.h"
#include "carreport_int.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* LVGL wants a millisecond clock (normally in hw.c) */
uint32_t nzeus_tick_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static long status_kb(const char *key)        /* a line of /proc/self/status, in kB */
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t n = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, n)) { v = atol(line + n + 1); break; }
    if (f) fclose(f);
    return v;
}

/* the characters of a UTF-8 string as numbers, for comparing */
static int codes(const char *s, uint32_t *out, int max)
{
    int n = 0;
    uint32_t c;
    while ((c = rp_utf8_next(&s)) != 0 && n < max) out[n++] = c;
    return n;
}

static int same_codes(const char *s, const uint32_t *want, int n)
{
    uint32_t got[64];
    int k = codes(s, got, 64);
    if (k != n) return 0;
    for (int i = 0; i < n; i++) if (got[i] != want[i]) return 0;
    return 1;
}

static void unit_checks(void)
{
    char out[512];
    /* joining */
    rp_shape_arabic("لا", out, sizeof out);
    check(same_codes(out, (uint32_t[]){0xFEFB}, 1), "Arabic: lam + alef become one sign (لا)");
    rp_shape_arabic("الآن", out, sizeof out);
    check(same_codes(out, (uint32_t[]){0xFE8D, 0xFEF5, 0xFEE5}, 3), "Arabic: الآن (now) joins with lam-alef-madda");
    rp_shape_arabic("سبتمبر", out, sizeof out);
    check(same_codes(out, (uint32_t[]){0xFEB3, 0xFE92, 0xFE98, 0xFEE4, 0xFE92, 0xFEAE}, 6),
          "Arabic: سبتمبر start / middle / end shapes");
    rp_shape_arabic("يُعبث", out, sizeof out);
    check(same_codes(out, (uint32_t[]){0xFEF3, 0x064F, 0xFECC, 0xFE92, 0xFE9A}, 5),
          "Arabic: a vowel mark does not break the joining");
    rp_shape_arabic("دار", out, sizeof out);
    check(same_codes(out, (uint32_t[]){0xFEA9, 0xFE8D, 0xFEAD}, 3), "Arabic: د ا ر never join forward");
    rp_shape_arabic("B10F3 ok", out, sizeof out);
    check(!strcmp(out, "B10F3 ok"), "Arabic joining leaves English alone");
    check(rp_shape_arabic("سبتمبر", out, 4) < 0, "Arabic joining refuses a too-small buffer");

    /* right-to-left order */
    const char *t = "س 12.2 V";
    rp_visual_order(t, (int)strlen(t), 1, out, sizeof out);
    check(!strcmp(out, "12.2 V س"), "RTL: number + unit stay together and read left to right");
    t = "(س)";
    rp_visual_order(t, (int)strlen(t), 1, out, sizeof out);
    check(!strcmp(out, "(س)"), "RTL: brackets are mirrored");
    t = "سص";
    rp_visual_order(t, (int)strlen(t), 1, out, sizeof out);
    check(!strcmp(out, "صس"), "RTL: Arabic letters are reversed for drawing");
    t = "a سص b";
    rp_visual_order(t, (int)strlen(t), 0, out, sizeof out);
    check(!strcmp(out, "a صس b"), "LTR paragraph: an Arabic word inside is reversed");
    t = "BdyCM / HCM";
    rp_visual_order(t, (int)strlen(t), 1, out, sizeof out);
    check(!strcmp(out, "BdyCM / HCM"), "RTL: English with signs inside keeps its order");
    t = "عُب";
    rp_visual_order(t, (int)strlen(t), 1, out, sizeof out);
    check(same_codes(out, (uint32_t[]){0x0628, 0x0639, 0x064F}, 3), "RTL: a vowel mark stays right after its letter");
}

static void fill_long(CarScan *s)       /* 60 codes in 5 groups, mixed states */
{
    carscan_sample(s);
    s->dtc_count = 60;
    static const char *g_en[5] = {"Exterior lights · Body Control Module", "Mirrors · Door modules",
                                  "Communication between modules", "Engine", "Brakes and stability"};
    static const char *g_ar[5] = {"الإضاءة الخارجية · وحدة التحكم بالهيكل", "المرايا · وحدات الأبواب",
                                  "الاتصال بين الوحدات", "المحرك", "الفرامل والثبات"};
    for (int i = 0; i < 60; i++) {
        ScanDtc *d = &s->dtcs[i];
        memset(d, 0, sizeof *d);
        int g = i / 12;
        snprintf(d->module, sizeof d->module, "%s", g == 3 ? "PCM" : g == 4 ? "ABS" : "BdyCM");
        snprintf(d->code, sizeof d->code, "%c%04X", g == 3 ? 'P' : g == 2 ? 'U' : 'B', 0x1000 + i * 7);
        d->state = i % 7 == 3 ? DTC_PENDING : DTC_HISTORY;
        snprintf(d->group_en, sizeof d->group_en, "%s", g_en[g]);
        snprintf(d->group_ar, sizeof d->group_ar, "%s", g_ar[g]);
        snprintf(d->meaning_en, sizeof d->meaning_en, "Test fault number %d: %s", i + 1,
                 i % 9 == 0 ? "a much longer explanation that has to wrap onto a second line inside the table cell" : "short circuit");
        snprintf(d->meaning_ar, sizeof d->meaning_ar, "عطل تجريبي رقم %d: %s", i + 1,
                 i % 9 == 0 ? "شرح أطول بكثير يجب أن ينتقل إلى سطر ثان داخل خانة الجدول" : "تماس في الدائرة");
    }
    s->healthy = 0;
    snprintf(s->headline_en, sizeof s->headline_en, "Check soon. 9 pending fault codes.");
    snprintf(s->headline_ar, sizeof s->headline_ar, "يحتاج متابعة. توجد 9 رسائل أعطال معلقة.");
}

static void fill_sick(CarScan *s)       /* active + pending codes, odometers that differ */
{
    carscan_sample(s);
    s->dtcs[0].state = DTC_ACTIVE;
    s->dtcs[4].state = DTC_PENDING;
    s->dtcs[8].state = DTC_PERMANENT;
    s->healthy = 0;
    s->mil_on = 1;
    s->engine_running = 1;
    s->battery_v = 14.1;
    s->odo_agree = 0;
    s->odo[1].km = 5120.0;
    snprintf(s->headline_en, sizeof s->headline_en, "Needs attention. 2 active faults.");
    snprintf(s->headline_ar, sizeof s->headline_ar, "يحتاج انتباه. يوجد عطلان حاليان.");
    snprintf(s->summary_en, sizeof s->summary_en, "The left front position light is failing right now and the "
             "engine computer reports a permanent code. The check-engine light is on.");
    snprintf(s->summary_ar, sizeof s->summary_ar, "إضاءة الموقع الأمامية اليسرى لا تعمل الآن، وكمبيوتر المحرك "
             "يسجل كودًا دائمًا. لمبة فحص المحرك مضاءة.");
}

static int make(const CarScan *s, const char *lang, const char *dir, char *path, int n)
{
    mkdir(dir, 0755);
    return carreport_pdf(s, lang, dir, path, n);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 1000; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_carreport <out folder>\n"); return 99; }
    const char *out = argv[1];
    setenv("TZ", "<+03>-3", 1);                    /* UTC+3: a time zone that isn't UTC */
    tzset();
    lv_init();
    mkdir(out, 0755);

    static CarScan s;                              /* (big: keep it off the stack) */
    char path[600], dir[600];

    /* 1) the sample, English: memory and time measured on the very first run */
    carscan_sample(&s);
    snprintf(dir, sizeof dir, "%s/sample", out);
    long hwm0 = status_kb("VmHWM:"), rss0 = status_kb("VmRSS:");
    carreport_mem_reset();
    double t0 = now_s();
    int r = make(&s, "en", dir, path, sizeof path);
    double t1 = now_s();
    long hwm1 = status_kb("VmHWM:");
    size_t peak = carreport_mem_peak();
    long grow = hwm1 - (hwm0 > rss0 ? hwm0 : rss0);
    printf("INFO English sample: %.2f s, %d pages, own memory peak %zu KB, process peak grew %ld KB\n",
           t1 - t0, carreport_last_pages(), peak / 1024, grow);
    check(r == 0 && exists(path), "English sample report written");
    check(strstr(path, "/Health-Scan-2026-09-29-EN.pdf") != NULL, "English file name Health-Scan-2026-09-29-EN.pdf");
    check(carreport_last_pages() == 3, "English sample has 3 pages, like the original");
    check(peak < 1200 * 1024, "memory peak of the report maker below 1.2 MB");
    check(grow < 3 * 1024, "whole process grew less than 3 MB while making the report");

    unit_checks();

    /* 2) Arabic */
    carreport_mem_reset();
    t0 = now_s();
    r = make(&s, "ar", dir, path, sizeof path);
    t1 = now_s();
    printf("INFO Arabic sample: %.2f s, %d pages, memory peak %zu KB\n", t1 - t0, carreport_last_pages(),
           carreport_mem_peak() / 1024);
    check(r == 0 && exists(path), "Arabic sample report written");
    check(strstr(path, "/Health-Scan-2026-09-29-AR.pdf") != NULL, "Arabic file name Health-Scan-2026-09-29-AR.pdf");
    check(carreport_last_pages() == 3, "Arabic sample has 3 pages, like the original");

    /* 3) the same name again gets -2, then -3 */
    char again[600];
    r = make(&s, "en", dir, again, sizeof again);
    check(r == 0 && strstr(again, "-EN-2.pdf") != NULL, "a second report the same day becomes ...-EN-2.pdf");
    r = make(&s, "en", dir, again, sizeof again);
    check(r == 0 && strstr(again, "-EN-3.pdf") != NULL, "a third one becomes ...-EN-3.pdf");
    remove(again);                                 /* tidy up: keep only EN + AR in sample/ */
    snprintf(again + strlen(again) - 5, 6, "2.pdf");
    remove(again);

    /* 4) bad calls */
    check(carreport_pdf(NULL, "en", out, path, sizeof path) == -1, "no scan -> -1");
    check(carreport_pdf(&s, "en", "/proc/nzeus-no-such/x", path, sizeof path) == -1 && path[0] == 0,
          "folder that can't be made -> -1, empty name");
    char tiny[10];
    check(carreport_pdf(&s, "en", dir, tiny, sizeof tiny) == -1, "too-small name buffer -> -1");

    /* 5) long scan: 60 codes */
    static CarScan L;
    fill_long(&L);
    snprintf(dir, sizeof dir, "%s/long", out);
    for (int a = 0; a < 2; a++) {
        r = make(&L, a ? "ar" : "en", dir, path, sizeof path);
        printf("INFO long scan %s: %d pages\n", a ? "AR" : "EN", carreport_last_pages());
        check(r == 0 && exists(path), a ? "long scan (60 codes), Arabic" : "long scan (60 codes), English");
        check(carreport_last_pages() >= 5, a ? "long Arabic scan flows over 5+ pages" : "long English scan flows over 5+ pages");
    }

    /* 6) DEMO data */
    static CarScan D;
    carscan_sample(&D);
    D.demo = 1;
    snprintf(dir, sizeof dir, "%s/demo", out);
    r = make(&D, "en", dir, path, sizeof path);
    check(r == 0 && exists(path), "DEMO report, English");
    r = make(&D, "ar", dir, path, sizeof path);
    check(r == 0 && exists(path), "DEMO report, Arabic");

    /* 7) an unhealthy car */
    static CarScan K;
    fill_sick(&K);
    snprintf(dir, sizeof dir, "%s/sick", out);
    r = make(&K, "en", dir, path, sizeof path);
    check(r == 0 && exists(path), "unhealthy car report, English");
    r = make(&K, "ar", dir, path, sizeof path);
    check(r == 0 && exists(path), "unhealthy car report, Arabic");

    /* 8) odd data: empty, garbage, huge words - no crash */
    static CarScan E;
    memset(&E, 0, sizeof E);
    snprintf(dir, sizeof dir, "%s/odd", out);
    r = make(&E, "en", dir, path, sizeof path);
    check(r == 0 && exists(path), "empty scan (all zero) still makes a report");
    r = make(&E, "ar", dir, path, sizeof path);
    check(r == 0 && exists(path), "empty scan in Arabic too");
    srand(7);
    unsigned char *raw = (unsigned char *)&E;
    for (size_t i = 0; i < sizeof E; i++) raw[i] = (unsigned char)rand();   /* garbage everywhere */
    r = make(&E, "en", dir, path, sizeof path);
    check(r == 0, "garbage scan (random bytes, no string ends) does not crash");
    r = make(&E, "ar", dir, path, sizeof path);
    check(r == 0, "garbage scan in Arabic does not crash");
    memset(&E, 0, sizeof E);
    carscan_sample(&E);
    memset(E.dtcs[0].meaning_en, 'W', sizeof E.dtcs[0].meaning_en - 1);   /* one huge word */
    memset(E.summary_en, 'x', sizeof E.summary_en - 1);
    E.dtc_count = -5; E.module_count = 1000; E.step_count = 99; E.wish_count = -1;
    E.battery_v = -3; E.odometer_km = 1e30; E.model_year = -7; E.odo_count = 50;
    r = make(&E, "xx", dir, path, sizeof path);
    check(r == 0, "silly counts and a 120-letter word don't crash (unknown language = English)");
    E.dtc_count = 1;
    r = make(&E, "en", dir, path, sizeof path);
    check(r == 0, "a 120-letter word inside a table cell is broken across lines");

    printf("%d checks failed\n", fails);
    return fails;
}
