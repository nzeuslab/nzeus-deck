/* carsum.c - the report's words, made from the scan data (see carsum.h).
 * The sentences follow a real FORScan "Raptor Health Scan" report of
 * 29 Sep 2026; the Arabic ones use that report's Arabic wording where it has
 * the same sentence. They work for any make: the Ford-only parts (Ford's code
 * meanings, the wishlist, "ask a Ford dealer") only appear for a Ford. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "carsum.h"
#include "dtc_db.h"

/* ---------- small text helpers ---------- */
/* cut a UTF-8 text back to a whole letter (an Arabic letter is 2 bytes) */
static void utf8_fix(char *s)
{
    size_t n = strlen(s);
    if (!n) return;
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;     /* back over continuation bytes */
    if (i == 0) return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = 0;                /* the last letter was cut: drop it */
}

/* add formatted text to the end of dst (never past its size, never half a letter) */
static void cat(char *dst, size_t size, const char *fmt, ...)
{
    size_t len = strlen(dst);
    if (len + 1 >= size) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst + len, size - len, fmt, ap);
    va_end(ap);
    utf8_fix(dst);
}
#define CAT(field, ...) cat(field, sizeof field, __VA_ARGS__)

/* Arabic counting: 1 and 2 have their own words, 3-10 take the plural,
 * 11 and up the singular ("3 رسائل" but "11 رسالة"). */
static void ar_count(char *out, size_t n, int count, const char *one, const char *two, const char *few, const char *many)
{
    int r = count % 100;
    if (count == 1) snprintf(out, n, "%s", one);
    else if (count == 2) snprintf(out, n, "%s", two);
    else snprintf(out, n, "%d %s", count, r >= 3 && r <= 10 ? few : many);
    utf8_fix(out);
}

static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); utf8_fix(dst); }

/* ---------- fault codes ---------- */
static int is_ford(const CarScan *s) { return !strcmp(s->make_en, "Ford"); }

/* a code's entry in dtc_db, for this car's make (Ford's wording only on a Ford) */
static const DtcInfo *lookup(const CarScan *s, const char *module, const char *code)
{
    return dtc_lookup_car(module, code, is_ford(s));
}

static const ScanModule *module_named(const CarScan *s, const char *code)
{
    int n = s->module_count < SCAN_MAX_MODULES ? s->module_count : SCAN_MAX_MODULES;
    for (int i = 0; i < n; i++) if (!strcmp(s->modules[i].code, code)) return &s->modules[i];
    return NULL;
}

ScanDtc *carscan_add_dtc(CarScan *s, const char *module, const char *code,
                         unsigned char failure_type, unsigned char status, int state)
{
    if (s->dtc_count >= SCAN_MAX_DTCS) return NULL;
    ScanDtc *d = &s->dtcs[s->dtc_count++];
    memset(d, 0, sizeof *d);
    copy(d->module, sizeof d->module, module);
    copy(d->code, sizeof d->code, code);
    d->failure_type = failure_type;
    d->status = status;
    d->state = state;
    /* the heading it is listed under: a Ford's codes by kind (lights, mirrors...,
     * like FORScan); any other make's codes by module ("Engine computer",
     * "Module 0x760"), because we can't tell what an unnamed module does */
    const ScanModule *m = is_ford(s) ? NULL : module_named(s, module);
    if (m && m->name_en[0]) {
        copy(d->group_en, sizeof d->group_en, m->name_en);
        copy(d->group_ar, sizeof d->group_ar, m->name_ar);
    } else {
        int g = dtc_group_of(module, code, is_ford(s));
        copy(d->group_en, sizeof d->group_en, dtc_group_name(g, "en"));
        copy(d->group_ar, sizeof d->group_ar, dtc_group_name(g, "ar"));
    }
    const DtcInfo *i = lookup(s, module, code);
    if (i) { copy(d->meaning_en, sizeof d->meaning_en, i->en); copy(d->meaning_ar, sizeof d->meaning_ar, i->ar); }
    return d;
}

ScanDtc *carscan_find_dtc(CarScan *s, const char *module, const char *code)
{
    for (int i = 0; i < s->dtc_count && i < SCAN_MAX_DTCS; i++)
        if (!strcmp(s->dtcs[i].module, module) && !strcmp(s->dtcs[i].code, code)) return &s->dtcs[i];
    return NULL;
}

static int has_code(const CarScan *s, const char *code)
{
    for (int i = 0; i < s->dtc_count; i++) if (!strcmp(s->dtcs[i].code, code)) return 1;
    return 0;
}

static int module_answered(const CarScan *s, const char *code)
{
    for (int i = 0; i < s->module_count; i++) if (s->modules[i].answered && !strcmp(s->modules[i].code, code)) return 1;
    return 0;
}

/* ---------- self-tests ---------- */
static const char *mon_ar(const char *en)
{
    static const char *const names[][2] = {
        { "Misfire", "خلل الاشتعال" }, { "Fuel system", "نظام الوقود" }, { "Components", "المكونات" },
        { "Catalyst", "دبة التلوث" }, { "Heated catalyst", "دبة التلوث المسخّنة" },
        { "Fuel vapour (EVAP)", "نظام أبخرة الوقود (EVAP)" }, { "Secondary air", "الهواء الثانوي" },
        { "A/C refrigerant", "غاز المكيف" }, { "Oxygen sensors", "حساسات الأكسجين" },
        { "Oxygen sensor heaters", "سخانات حساسات الأكسجين" }, { "EGR / VVT", "EGR / VVT" },
        { "NMHC catalyst", "محفز NMHC" }, { "NOx / SCR", "NOx / SCR" }, { "Boost pressure", "ضغط التيربو" },
        { "Exhaust gas sensor", "حساس غازات العادم" }, { "Particulate filter", "فلتر الجزيئات (DPF)" },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(names[i][0], en)) return names[i][1];
    return en;
}

void carscan_set_readiness(CarScan *s, const ObdInfo *I)
{
    s->readiness_en[0] = s->readiness_ar[0] = 0;
    s->readiness_total = s->readiness_ready = 0;
    if (!I->have_status) return;
    s->mil_known = 1;
    s->mil_on = I->mil_on;
    s->readiness_total = I->mon_count;
    for (int i = 0; i < I->mon_count; i++) s->readiness_ready += I->mon[i].complete;
    if (s->readiness_ready == s->readiness_total) {
        CAT(s->readiness_en, "All %d self-tests done", s->readiness_total);
        CAT(s->readiness_ar, "اكتملت جميع الاختبارات الذاتية (%d)", s->readiness_total);
        return;
    }
    CAT(s->readiness_en, "%d of %d self-tests done; not yet: ", s->readiness_ready, s->readiness_total);
    CAT(s->readiness_ar, "اكتمل %d من %d اختبارات ذاتية؛ لم يكتمل بعد: ", s->readiness_ready, s->readiness_total);
    int first = 1;
    for (int i = 0; i < I->mon_count; i++) {
        if (I->mon[i].complete) continue;
        CAT(s->readiness_en, "%s%s", first ? "" : ", ", I->mon[i].name);
        CAT(s->readiness_ar, "%s%s", first ? "" : "، ", mon_ar(I->mon[i].name));
        first = 0;
    }
}

/* ---------- the VIN ---------- */
int vin_model_year(const char *vin)
{
    /* position 10: A=2010 ... Y=2030 (no I O Q U Z), then 1-9 = 2001-2009
     * (the same letters meant 1980-2000 before; a car that reads OBD-II
     * with a 17-letter VIN through this app is assumed 2001 or newer) */
    static const char letters[] = "ABCDEFGHJKLMNPRSTVWXY";
    if (!vin || strlen(vin) < 10) return 0;
    char c = vin[9];
    const char *p = strchr(letters, c);
    if (p && c) return 2010 + (int)(p - letters);
    if (c >= '1' && c <= '9') return 2000 + (c - '0');
    return 0;
}

void vin_make(const char *vin, CarScan *s)
{
    /* World Manufacturer Identifiers (the VIN's first 3 letters) of common
     * makes - only well-known ones; anything else stays "unknown".
     * Ford: USA, Canada, Mexico, Australia, Thailand, South Africa, Germany. */
    static const struct { const char *wmi, *en, *ar; } makes[] = {
        { "1FA 1FB 1FC 1FD 1FM 1FT 1FV 2FA 2FM 2FT 3FA 3FM 3FT 6FP MNA MPB AFA WF0 MAJ NM0", "Ford", "فورد" },
        { "JTD JTE JTK JTL JTM JTN JT2 JT3 4T1 4T3 4T4 5TD 5TF 5TB 2T1 2T3 MR0 MHF AHT SB1 NMT 6T1 VNK", "Toyota", "تويوتا" },
        { "JTH JTJ 2T2 58A", "Lexus", "لكزس" },
        { "JN1 JN8 1N4 1N6 3N1 5N1 SJN VSK MNT", "Nissan", "نيسان" },
        { "JNK JNR 5N3", "Infiniti", "إنفينيتي" },
        { "JHM JHL 1HG 2HG 2HK 5FN 5J6 SHH MRH", "Honda", "هوندا" },
        { "JH4 19U 5J8", "Acura", "أكيورا" },
        { "KMH KM8 5NP 5NM MAL TMA", "Hyundai", "هيونداي" },
        { "KNA KND 5XY 5XX U5Y 3KP", "Kia", "كيا" },
        { "1G1 1GC 1GN 2G1 3GC 3GN", "Chevrolet", "شيفروليه" },
        { "1GT 3GT 1GK", "GMC", "جي إم سي" },
        { "1G6 1GY", "Cadillac", "كاديلاك" },
        { "JA3 JA4 JMB JMY MMB 4A3", "Mitsubishi", "ميتسوبيشي" },
        { "JM1 JM3 JMZ", "Mazda", "مازدا" },
        { "JF1 JF2 4S3 4S4", "Subaru", "سوبارو" },
        { "JS2 JS3 MA3", "Suzuki", "سوزوكي" },
        { "JAA JAL MPA", "Isuzu", "إيسوزو" },
        { "WBA WBS WBX WBY 5UX 5YM", "BMW", "بي إم دبليو" },
        { "WMW", "MINI", "ميني" },
        { "WDB WDC WDD WDF W1K W1N 4JG 55S", "Mercedes-Benz", "مرسيدس بنز" },
        { "WVW WVG WV1 WV2 1VW 3VW", "Volkswagen", "فولكس فاجن" },
        { "WAU WA1 WUA", "Audi", "أودي" },
        { "WP0 WP1", "Porsche", "بورش" },
        { "SAL", "Land Rover", "لاند روفر" },
        { "SAJ", "Jaguar", "جاكوار" },
        { "YV1 YV4", "Volvo", "فولفو" },
        { "VF1", "Renault", "رينو" },
        { "VF3", "Peugeot", "بيجو" },
        { "VF7", "Citroen", "ستروين" },
        { "5YJ 7SA", "Tesla", "تسلا" },
        { "1J4 1J8", "Jeep", "جيب" },
    };
    s->make_en[0] = s->make_ar[0] = 0;
    s->make_model_en[0] = s->make_model_ar[0] = 0;
    if (!vin || strlen(vin) < 3) return;
    for (size_t i = 0; i < sizeof makes / sizeof makes[0]; i++)
        for (const char *w = makes[i].wmi; *w; w += w[3] ? 4 : 3)     /* "1FA 1FB ...": 3 letters + a space each */
            if (!strncmp(vin, w, 3)) {
                copy(s->make_en, sizeof s->make_en, makes[i].en);
                copy(s->make_ar, sizeof s->make_ar, makes[i].ar);
                copy(s->make_model_en, sizeof s->make_model_en, makes[i].en);   /* (the model can't be told from the VIN) */
                copy(s->make_model_ar, sizeof s->make_model_ar, makes[i].ar);
                return;
            }
}

/* ---------- the mileage check ---------- */
void carscan_check_odometer(CarScan *s)
{
    if (s->odo_count == 0) { s->odo_agree = -1; s->odometer_km = -1; return; }
    double lo = s->odo[0].km, hi = s->odo[0].km, shown = -1;
    for (int i = 0; i < s->odo_count; i++) {
        if (s->odo[i].km < lo) lo = s->odo[i].km;
        if (s->odo[i].km > hi) hi = s->odo[i].km;
        if (!strcmp(s->odo[i].module, "IPC")) shown = s->odo[i].km;    /* what the dashboard shows */
    }
    /* winding an odometer back lowers it, so if they differ the highest is the honest one */
    s->odometer_km = shown >= 0 ? shown : hi;
    if (s->odo_count < 2) { s->odo_agree = -1; return; }
    s->odo_agree = hi - lo <= 1.0;                        /* all the same, within 1 km */
}

/* ---------- the wishlist (only from which modules answered, like his report) ---------- */
static void wish(CarScan *s, const char *fen, const char *far, const char *men, const char *mar, int outlook)
{
    if (s->wish_count >= SCAN_MAX_WISH) return;
    ScanWish *w = &s->wish[s->wish_count++];
    copy(w->feature_en, sizeof w->feature_en, fen);
    copy(w->feature_ar, sizeof w->feature_ar, far);
    copy(w->module_en, sizeof w->module_en, men);
    copy(w->module_ar, sizeof w->module_ar, mar);
    w->outlook = outlook;
}

static void make_wishlist(CarScan *s)
{
    s->wish_count = 0;
    if (!is_ford(s)) return;                              /* Ford's modules and FORScan settings only */
    int bcm = module_answered(s, "BdyCM"), hcm = module_answered(s, "HCM"), ipc = module_answered(s, "IPC");
    int ipma = module_answered(s, "IPMA"), apim = module_answered(s, "APIM");
    if (module_answered(s, "TCU"))
        wish(s, "Wi-Fi hotspot", "نقطة اتصال واي فاي", "TCU (modem) present", "وحدة الاتصال TCU موجودة", 0);
    if (module_answered(s, "RTM"))
        wish(s, "Remote start", "التشغيل عن بعد", "RTM (key-fob receiver) present", "مستقبل الريموت RTM موجود", 0);
    if (ipc)
        wish(s, "Digital gauges (trans/engine temp)", "عدادات رقمية (حرارة القير والمحرك)", "IPC (cluster) present", "العدادات IPC موجودة", 0);
    if (ipma && module_answered(s, "PSCM"))
        wish(s, "Steering assist (lane keeping)", "مساعد التوجيه (البقاء في المسار)", "IPMA camera + PSCM steering present", "الكاميرا IPMA + المقود PSCM", 0);
    if (ipma && ipc)
        wish(s, "Traffic sign on cluster", "عرض إشارات السرعة على العدادات", "IPMA camera + IPC present", "الكاميرا IPMA + العدادات IPC", 0);
    if (bcm) {
        const char *drl = hcm ? "BdyCM / HCM" : "BdyCM";
        wish(s, "Headlights always in AUTO", "الأنوار دائمًا على AUTO", "BdyCM", "BdyCM", 1);
        wish(s, "DRL off on indicating side", "إطفاء ضوء النهار في جهة الإشارة", drl, drl, 1);
        wish(s, "DRL fully off option", "خيار إطفاء ضوء النهار بالكامل", drl, drl, 1);
        wish(s, "Seatbelt chime off", "إلغاء تنبيه حزام الأمان", ipc ? "BdyCM / IPC" : "BdyCM", ipc ? "BdyCM / IPC" : "BdyCM", 2);
    }
    if (apim || ipma) {
        const char *m = apim && ipma ? "APIM / IPMA" : apim ? "APIM" : "IPMA";
        wish(s, "Rear camera text overlay off", "إزالة النص على كاميرا الرجوع", m, m, 3);
    }
}

/* ---------- the top of the report ---------- */
static void add_step(CarScan *s, const char *en, const char *ar)
{
    if (s->step_count >= SCAN_MAX_STEPS) return;
    copy(s->steps_en[s->step_count], sizeof s->steps_en[0], en);
    copy(s->steps_ar[s->step_count], sizeof s->steps_ar[0], ar);
    s->step_count++;
}

/* Paired stored codes can suggest a shared cause, but contain no event time. */
static int power_event(const CarScan *s)
{
    for (int i = 0; i < s->dtc_count; i++) {
        const DtcInfo *d = lookup(s, s->dtcs[i].module, s->dtcs[i].code);
        if (d && d->pair && s->dtcs[i].state != DTC_ACTIVE && has_code(s, d->pair)) return 1;
    }
    return 0;
}

static int is_core(const char *m)        /* engine, gearbox, brakes, airbags, steering */
{
    static const char *const core[] = { "PCM", "TCM", "ABS", "VDM", "RCM", "PSCM", "ECM", "OBD" };
    for (size_t i = 0; i < sizeof core / sizeof core[0]; i++) if (!strcmp(m, core[i])) return 1;
    return 0;
}

static void battery_step(CarScan *s)
{
    char en[200], ar[320];
    double v = s->battery_v;
    if (v <= 0) return;
    if (v < 12.0) {
        snprintf(en, sizeof en, "Battery at %.1f V is low. Charge it (or drive for 30 minutes) and scan again: low voltage can cause false codes.", v);
        snprintf(ar, sizeof ar, "البطارية على %.1f V منخفضة. اشحنها (أو امشِ بالسيارة 30 دقيقة) وأعد الفحص، فالجهد المنخفض قد يسبب رسائل خطأ غير حقيقية.", v);
    } else if (v < 12.5) {
        snprintf(en, sizeof en, "Battery at %.1f V is fine for reading. Before changing any settings, connect a battery charger or power supply so it stays above 12.5 V.", v);
        snprintf(ar, sizeof ar, "البطارية على %.1f V مناسبة للقراءة فقط. قبل تغيير أي إعداد، نوصل شاحن بطارية حتى يبقى الجهد فوق 12.5 V.", v);
    } else if (v < 13.0) {
        snprintf(en, sizeof en, "Battery at %.1f V is good. Before changing any settings, still connect a charger so it stays above 12.5 V.", v);
        snprintf(ar, sizeof ar, "البطارية على %.1f V جيدة. قبل تغيير أي إعداد، نوصل شاحن بطارية حتى يبقى الجهد فوق 12.5 V.", v);
    } else if (v <= 15.0) {
        snprintf(en, sizeof en, "Battery at %.1f V: the engine is running and the alternator is charging normally.", v);
        snprintf(ar, sizeof ar, "البطارية على %.1f V: المحرك يعمل والدينمو يشحن بشكل طبيعي.", v);
    } else {
        snprintf(en, sizeof en, "Battery at %.1f V is too high: have the charging system (alternator) checked.", v);
        snprintf(ar, sizeof ar, "البطارية على %.1f V مرتفعة جدًا: افحص نظام الشحن (الدينمو).", v);
    }
    add_step(s, en, ar);
}

/* the codes in the report's order: a Ford's by group (lights, mirrors,
 * communication...), keeping the scan's order inside a group. Other makes
 * keep the scan's order (module by module). */
static void sort_by_group(CarScan *s)
{
    static ScanDtc tmp;                                   /* (big: not on the stack) */
    if (!is_ford(s)) return;
    for (int i = 1; i < s->dtc_count; i++) {
        int g = dtc_group_of(s->dtcs[i].module, s->dtcs[i].code, 1), k = i;
        tmp = s->dtcs[i];
        while (k > 0 && dtc_group_of(s->dtcs[k - 1].module, s->dtcs[k - 1].code, 1) > g) { s->dtcs[k] = s->dtcs[k - 1]; k--; }
        s->dtcs[k] = tmp;
    }
}

void carscan_summarize(CarScan *s, int second_bus_unreached)
{
    int active = 0, pending = 0, history = 0, perm = 0, stored = 0, core_codes = 0;
    int ford = is_ford(s);
    sort_by_group(s);
    for (int i = 0; i < s->dtc_count; i++) {
        switch (s->dtcs[i].state) {
        case DTC_ACTIVE:    active++; break;
        case DTC_PENDING:   pending++; break;
        case DTC_PERMANENT: perm++; break;
        case DTC_STORED:    stored++; break;
        default:            history++; break;
        }
        if (is_core(s->dtcs[i].module)) core_codes++;
    }
    int total = s->dtc_count;
    s->modules_answered = 0;
    int codes_read = 0;
    for (int i = 0; i < s->module_count; i++) {
        s->modules_answered += s->modules[i].answered;
        codes_read += s->modules[i].answered && s->modules[i].codes_read;
    }
    int incomplete = !s->modules_answered || !s->mil_known ||
                     codes_read < s->modules_answered || second_bus_unreached;
    carscan_check_odometer(s);
    /* healthy = nothing failing now, nothing waiting, no confirmed engine code, light off */
    s->healthy = !incomplete && !active && !pending && !stored && !s->mil_on;

    /* headline */
    s->headline_en[0] = s->headline_ar[0] = 0;
    char n_ar[96];
    if (incomplete && !active && !pending && !stored && !s->mil_on) {
        CAT(s->headline_en, "Scan incomplete: some results are unknown.");
        CAT(s->headline_ar, "الفحص غير مكتمل: بعض النتائج غير معروفة.");
    }
    else if (s->healthy && !total) { CAT(s->headline_en, "No fault codes returned."); CAT(s->headline_ar, "لم تُرجع الوحدات رسائل أعطال."); }
    else if (s->healthy) { CAT(s->headline_en, "No active faults reported."); CAT(s->headline_ar, "لم تُبلّغ أعطال حالية."); }
    else if (active) {
        CAT(s->headline_en, "Needs a check: %d active fault%s.", active, active == 1 ? "" : "s");
        ar_count(n_ar, sizeof n_ar, active, "عطل حالي واحد", "عطلان حاليان", "أعطال حالية", "عطلًا حاليًا");
        CAT(s->headline_ar, "تحتاج إلى فحص: %s.", n_ar);
    } else if (stored) {
        CAT(s->headline_en, "Needs a check: %d stored engine fault code%s.", stored, stored == 1 ? "" : "s");
        ar_count(n_ar, sizeof n_ar, stored, "رسالة خطأ مخزنة واحدة", "رسالتا خطأ مخزنتان", "رسائل خطأ مخزنة", "رسالة خطأ مخزنة");
        CAT(s->headline_ar, "تحتاج إلى فحص: %s.", n_ar);
    } else if (s->mil_on) { CAT(s->headline_en, "The check-engine light is on."); CAT(s->headline_ar, "لمبة فحص المحرك مضاءة."); }
    else { CAT(s->headline_en, "Pending fault codes need checking."); CAT(s->headline_ar, "رسائل الأعطال المعلّقة تحتاج إلى فحص."); }

    /* summary */
    s->summary_en[0] = s->summary_ar[0] = 0;
    if (s->modules_answered) {
        CAT(s->summary_en, "%d module%s answered. ", s->modules_answered, s->modules_answered == 1 ? "" : "s");
        ar_count(n_ar, sizeof n_ar, s->modules_answered, "وحدة إلكترونية واحدة", "وحدتان إلكترونيتان", "وحدات إلكترونية", "وحدة إلكترونية");
        CAT(s->summary_ar, "استجابت %s. ", n_ar);
    } else {
        CAT(s->summary_en, "No module answered (is the ignition on?). ");
        CAT(s->summary_ar, "لم تستجب أي وحدة (هل السويتش على ON؟). ");
    }
    if (!total) {
        if (codes_read) {
            CAT(s->summary_en, "No fault codes were returned by the modules read. ");
            CAT(s->summary_ar, "لم تُرجع الوحدات التي قُرئت أي رسائل أعطال. ");
        } else {
            CAT(s->summary_en, "No fault-code results were received. ");
            CAT(s->summary_ar, "لم تصل أي نتائج لرسائل الأعطال. ");
        }
    } else if (!active && !pending && !stored) {
        if (history == 1) {
            CAT(s->summary_en, "There is 1 old (history) code, but it is not happening now");
            CAT(s->summary_ar, "توجد رسالة خطأ قديمة واحدة، لكنها غير موجودة الآن");
        } else if (history) {
            ar_count(n_ar, sizeof n_ar, history, "رسالة خطأ قديمة واحدة", "رسالتا خطأ قديمتان", "رسائل خطأ قديمة", "رسالة خطأ قديمة");
            CAT(s->summary_en, "There are %d old (history) codes, but none of them is happening now", history);
            CAT(s->summary_ar, "توجد %s، لكن لا يوجد أي منها الآن", n_ar);
        }
        if (history && s->mil_known && !s->mil_on) { CAT(s->summary_en, ", and the check-engine light is off. "); CAT(s->summary_ar, "، ولمبة فحص المحرك مطفأة. "); }
        else if (history) { CAT(s->summary_en, ". "); CAT(s->summary_ar, ". "); }
        if (perm) {
            CAT(s->summary_en, "%d permanent code%s will clear by itself once the car's self-tests pass. ", perm, perm == 1 ? "" : "s");
            CAT(s->summary_ar, "الرسائل الدائمة (%d) تُمسح تلقائيًا بعد نجاح الاختبارات الذاتية للسيارة. ", perm);
        }
    } else if (stored) {
        CAT(s->summary_en, "Stored engine codes (confirmed, may still be there): %d. Active now: %d. Pending: %d. Old (history): %d. ",
            stored, active, pending, history + perm);
        CAT(s->summary_ar, "رسائل المحرك المخزنة (مؤكدة وقد تكون ما زالت موجودة): %d. الأعطال الحالية: %d. المعلّقة: %d. القديمة: %d. ",
            stored, active, pending, history + perm);
    } else {
        CAT(s->summary_en, "Active now: %d. Pending: %d. Old (history): %d. ", active, pending, history + perm);
        CAT(s->summary_ar, "الأعطال الحالية: %d. المعلّقة: %d. القديمة: %d. ", active, pending, history + perm);
    }
    if (s->mil_on) { CAT(s->summary_en, "The check-engine light is ON. "); CAT(s->summary_ar, "لمبة فحص المحرك مضاءة. "); }
    if (ford && core_codes) {
        CAT(s->summary_en, "Some codes come from the engine, gearbox, brakes, airbags or steering: see the list.");
        CAT(s->summary_ar, "بعض الرسائل من المحرك أو ناقل الحركة أو الفرامل أو الوسائد الهوائية أو المقود: راجع القائمة.");
    } else if (!ford && core_codes) {
        CAT(s->summary_en, "Some codes come from the engine or gearbox computer: see the list.");
        CAT(s->summary_ar, "بعض الرسائل من كمبيوتر المحرك أو ناقل الحركة: راجع القائمة.");
    }
    if (incomplete) {
        CAT(s->summary_en, " Unread modules and missing results remain unknown.");
        CAT(s->summary_ar, " تبقى الوحدات التي لم تُقرأ والنتائج الناقصة غير معروفة.");
    }
    if (second_bus_unreached && ford) {
        CAT(s->summary_en, " Modules on the second bus (MS-CAN) could not be reached with this adapter.");
        CAT(s->summary_ar, " تعذّر الوصول إلى الوحدات على الشبكة الثانية (MS-CAN) بهذا الجهاز.");
    }

    /* most likely cause */
    s->cause_en[0] = s->cause_ar[0] = 0;
    if (power_event(s)) {
        CAT(s->cause_en, "Possible shared cause: a power event, such as a disconnected or low battery. Paired codes "
                         "do not show when the faults happened or rule out bulbs and wiring; inspect before diagnosing.");
        CAT(s->cause_ar, "سبب مشترك محتمل: مشكلة كهرباء مؤقتة، مثل فصل البطارية أو انخفاض جهدها. الأكواد المتقابلة "
                         "لا تحدد وقت حدوث الأعطال ولا تستبعد تلف اللمبات أو الأسلاك؛ افحص قبل التشخيص.");
    } else if (total && s->battery_v > 0 && s->battery_v < 12.0) {
        CAT(s->cause_en, "Observed supply voltage: %.1f V. Check the battery and power supply, and investigate the fault codes separately; "
                         "this reading does not establish their cause.", s->battery_v);
        CAT(s->cause_ar, "جهد التغذية المقاس: %.1f V. افحص البطارية والتغذية الكهربائية، وتحقق من أسباب رسائل الأعطال بشكل منفصل؛ "
                         "هذه القراءة لا تثبت سببها.", s->battery_v);
    }

    /* what to do */
    s->step_count = 0;
    char en[200] = "", ar[320] = "";
    const char *items_en[8], *items_ar[8];
    int items = 0;
    for (int i = 0; i < s->dtc_count && items < 8; i++) {          /* things to look at, once each */
        const DtcInfo *d = lookup(s, s->dtcs[i].module, s->dtcs[i].code);
        if (!d || !d->check_en) continue;
        int seen = 0;
        for (int k = 0; k < items; k++) if (!strcmp(items_en[k], d->check_en)) seen = 1;
        if (!seen) { items_en[items] = d->check_en; items_ar[items++] = d->check_ar; }
    }
    if (items) {
        CAT(en, "Walk around the car and check that the ");
        CAT(ar, "لف حول السيارة وتأكد أن ");
        for (int k = 0; k < items; k++) {
            CAT(en, "%s%s", k == 0 ? "" : k == items - 1 ? " and " : ", ", items_en[k]);
            CAT(ar, "%s%s", k == 0 ? "" : k == items - 1 ? "، و" : "، ", items_ar[k]);
        }
        CAT(en, "%s", items > 1 ? " all work." : " work.");
        CAT(ar, "%s", items > 1 ? " كلها تعمل." : " تعمل.");
        add_step(s, en, ar);
    }
    if (active) {
        en[0] = ar[0] = 0;
        CAT(en, "Have the active faults checked first:");
        CAT(ar, "افحص الأعطال الحالية أولًا:");
        for (int i = 0; i < s->dtc_count; i++)
            if (s->dtcs[i].state == DTC_ACTIVE) { CAT(en, " %s", s->dtcs[i].code); CAT(ar, " %s", s->dtcs[i].code); }
        CAT(en, ". Do not clear them before they are fixed.");
        CAT(ar, ". لا تمسحها قبل إصلاحها.");
        add_step(s, en, ar);
    }
    if (stored) {
        en[0] = ar[0] = 0;
        CAT(en, "Have the stored engine codes checked:");
        CAT(ar, "افحص رسائل المحرك المخزنة:");
        for (int i = 0; i < s->dtc_count; i++)
            if (s->dtcs[i].state == DTC_STORED) { CAT(en, " %s", s->dtcs[i].code); CAT(ar, " %s", s->dtcs[i].code); }
        CAT(en, ". The engine computer confirmed each one; fix the cause before clearing anything.");
        CAT(ar, ". كمبيوتر المحرك أكّد كل واحدة منها؛ أصلح السبب قبل مسح أي شيء.");
        add_step(s, en, ar);
    }
    if (s->mil_on)
        add_step(s, "The check-engine light is on: read the engine codes and fix the cause before clearing anything.",
                 "لمبة فحص المحرك مضاءة: اقرأ أكواد المحرك وأصلح السبب قبل مسح أي شيء.");
    if (history)
        add_step(s, items ? "If everything works, clear these history codes (with approval) and scan again after a few drives. Any code that comes back is a real issue."
                          : "Clear these history codes (with approval) and scan again after a few drives. Any code that comes back is a real issue.",
                 items ? "إذا كان كل شيء يعمل، نمسح هذه الأعطال القديمة (بعد الموافقة) ونعيد الفحص بعد عدة مشاوير. أي كود يرجع يعني مشكلة حقيقية."
                       : "نمسح هذه الأعطال القديمة (بعد الموافقة) ونعيد الفحص بعد عدة مشاوير. أي كود يرجع يعني مشكلة حقيقية.");
    if (s->odo_agree == 0) {
        double lo = s->odo[0].km, hi = s->odo[0].km;
        for (int i = 0; i < s->odo_count; i++) { if (s->odo[i].km < lo) lo = s->odo[i].km; if (s->odo[i].km > hi) hi = s->odo[i].km; }
        snprintf(en, sizeof en, "The modules do not agree on the mileage (%.0f km vs %.0f km). Ask %s dealer to check the odometer history.",
                 lo, hi, ford ? "a Ford" : "the");
        snprintf(ar, sizeof ar, "الوحدات لا تتفق على قراءة الممشى (%.0f كم مقابل %.0f كم). اطلب من %s فحص سجل العداد.",
                 lo, hi, ford ? "وكيل فورد" : "الوكيل");
        add_step(s, en, ar);
    }
    battery_step(s);
    if (pending)
        add_step(s, "Codes waiting to be confirmed: drive normally for a few days and scan again; if they come back as active, have them checked.",
                 "رسائل بانتظار التأكيد: استخدم السيارة بشكل طبيعي لعدة أيام وأعد الفحص، وإذا أصبحت أعطالًا حالية افحصها.");

    make_wishlist(s);
}
