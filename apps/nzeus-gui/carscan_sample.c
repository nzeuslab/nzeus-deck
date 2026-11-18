/* carscan_sample.c - a ready-made scan for the report's PC tests only (the
 * Car app never shows it: with no adapter there is no report). The data of the
 * 29 Sep 2026 "Raptor Health Scan" reports (2025 Ford Ranger Raptor, 764 km,
 * 28 modules, 11 old fault codes, nothing active).
 * The middle of the VIN is made up (the reports only show 6FP·········00001),
 * and the CAN addresses are the usual Ford ones, for show only. */
#include <stdio.h>
#include <string.h>
#include "carscan.h"

/* copy text into a fixed-size field, always ending it properly */
#define SET(field, text) snprintf((field), sizeof(field), "%s", (text))

static const struct { const char *code, *en, *ar; int id; } MODULES[] = {
    {"PCM",   "Powertrain (engine) control", "كمبيوتر المحرك",            0x7E0},
    {"ABS",   "Anti-lock brakes",            "نظام منع انغلاق الفرامل",    0x760},
    {"PSCM",  "Power steering",              "المقود الكهربائي",           0x730},
    {"VDM",   "Vehicle dynamics",            "ثبات السيارة",               0x7B0},
    {"AWD",   "All-wheel drive",             "الدفع الرباعي",              0x703},
    {"ATCM",  "All-terrain control",         "أنماط القيادة للطرق الوعرة", 0x7B3},
    {"GSM",   "Gear shift",                  "ذراع القير",                 0x732},
    {"CCM",   "Cruise control (radar)",      "مثبت السرعة (رادار)",        0x764},
    {"IPMA",  "Front camera",                "الكاميرا الأمامية",          0x706},
    {"SODL",  "Blind spot, left",            "النقطة العمياء، يسار",       0x7C4},
    {"SODR",  "Blind spot, right",           "النقطة العمياء، يمين",       0x7C6},
    {"RCM",   "Airbags",                     "الوسائد الهوائية",           0x737},
    {"BdyCM", "Body control",                "التحكم بالهيكل",             0x726},
    {"BCMC",  "Body control C",              "التحكم بالهيكل C",           0x7D3},
    {"HCM",   "Headlamps",                   "المصابيح الأمامية",          0x734},
    {"DDM",   "Driver door",                 "باب السائق",                 0x740},
    {"PDM",   "Passenger door",              "باب الراكب",                 0x741},
    {"HVAC",  "Air conditioning",            "المكيف",                     0x733},
    {"IPC",   "Instrument cluster",          "العدادات",                   0x720},
    {"SCCM",  "Steering column switches",    "مفاتيح عمود المقود",         0x724},
    {"FCIM",  "Front controls",              "أزرار التحكم الأمامية",      0x7A7},
    {"APIM",  "SYNC screen",                 "شاشة SYNC",                  0x7D0},
    {"ACM",   "Audio control",               "التحكم بالصوت",              0x727},
    {"DSP",   "Audio amplifier",             "مضخم الصوت",                 0x783},
    {"TCU",   "Modem / telematics",          "وحدة الاتصال (مودم)",        0x754},
    {"RTM",   "Key-fob receiver",            "مستقبل الريموت",             0x751},
    {"WACM",  "Wireless phone charger",      "الشاحن اللاسلكي",            0x7C5},
    {"GWM",   "Gateway",                     "البوابة (Gateway)",          0x716},
};

#define G_LIGHTS_EN "Exterior lights · Body Control Module"
#define G_LIGHTS_AR "الإضاءة الخارجية · وحدة التحكم بالهيكل"
#define G_MIRROR_EN "Mirrors · Door modules"
#define G_MIRROR_AR "المرايا · وحدات الأبواب"
#define G_COMMS_EN  "Communication between modules"
#define G_COMMS_AR  "الاتصال بين الوحدات"

static const struct { const char *mod, *code, *gen, *gar, *en, *ar; } DTCS[] = {
    {"BdyCM", "B10F3", G_LIGHTS_EN, G_LIGHTS_AR, "Left front position light: open or short circuit",
     "إضاءة الموقع الأمامية اليسرى: انقطاع أو تماس في الدائرة"},
    {"BdyCM", "B10F4", G_LIGHTS_EN, G_LIGHTS_AR, "Right front position light: open or short circuit",
     "إضاءة الموقع الأمامية اليمنى: انقطاع أو تماس في الدائرة"},
    {"BdyCM", "B1444", G_LIGHTS_EN, G_LIGHTS_AR, "Left rear stop/position lamp: open or short circuit",
     "لمبة الفرامل/الموقع الخلفية اليسرى: انقطاع أو تماس"},
    {"BdyCM", "B1448", G_LIGHTS_EN, G_LIGHTS_AR, "Right rear stop/position lamp: open or short circuit",
     "لمبة الفرامل/الموقع الخلفية اليمنى: انقطاع أو تماس"},
    {"DDM", "B1163", G_MIRROR_EN, G_MIRROR_AR, "Left mirror heater: open or short circuit",
     "سخان المرآة اليسرى: انقطاع أو تماس"},
    {"DDM", "B1165", G_MIRROR_EN, G_MIRROR_AR, "Left puddle lamp (under mirror): open or short circuit",
     "إضاءة الأرضية تحت المرآة اليسرى: انقطاع أو تماس"},
    {"PDM", "B1164", G_MIRROR_EN, G_MIRROR_AR, "Right mirror heater: open or short circuit",
     "سخان المرآة اليمنى: انقطاع أو تماس"},
    {"PDM", "B1166", G_MIRROR_EN, G_MIRROR_AR, "Right puddle lamp (under mirror): open or short circuit",
     "إضاءة الأرضية تحت المرآة اليمنى: انقطاع أو تماس"},
    {"HCM", "U0122", G_COMMS_EN, G_COMMS_AR, "Headlamp module lost contact with the stability-control module",
     "وحدة المصابيح الأمامية فقدت الاتصال بوحدة التحكم بالثبات"},
    {"HVAC", "U0401", G_COMMS_EN, G_COMMS_AR, "A/C module got invalid data from the engine computer",
     "وحدة المكيف استلمت بيانات غير صحيحة من كمبيوتر المحرك"},
    {"ACM", "B150C", G_COMMS_EN, G_COMMS_AR, "Audio system: a speaker-network message went missing",
     "نظام الصوت: رسالة مفقودة في شبكة السماعات"},
};

static const struct { const char *fen, *far, *men, *mar; int outlook; } WISH[] = {
    {"Wi-Fi hotspot", "نقطة اتصال واي فاي", "TCU (modem) present", "وحدة الاتصال TCU موجودة", 0},
    {"Remote start", "التشغيل عن بعد", "RTM (key-fob receiver) present", "مستقبل الريموت RTM موجود", 0},
    {"Digital gauges (trans/engine temp)", "عدادات رقمية (حرارة القير والمحرك)",
     "IPC (cluster) present", "العدادات IPC موجودة", 0},
    {"Steering assist (lane keeping)", "مساعد التوجيه (البقاء في المسار)",
     "IPMA camera + PSCM steering present", "الكاميرا IPMA + المقود PSCM", 0},
    {"Traffic sign on cluster", "عرض إشارات السرعة على العدادات",
     "IPMA camera + IPC present", "الكاميرا IPMA + العدادات IPC", 0},
    {"Headlights always in AUTO", "الأنوار دائمًا على AUTO", "BdyCM", "BdyCM", 1},
    {"DRL off on indicating side", "إطفاء ضوء النهار في جهة الإشارة", "BdyCM / HCM", "BdyCM / HCM", 1},
    {"DRL fully off option", "خيار إطفاء ضوء النهار بالكامل", "BdyCM / HCM", "BdyCM / HCM", 1},
    {"Seatbelt chime off", "إلغاء تنبيه حزام الأمان", "BdyCM / IPC", "BdyCM / IPC", 2},
    {"Rear camera text overlay off", "إزالة النص على كاميرا الرجوع", "APIM / IPMA", "APIM / IPMA", 3},
};

static const char *STEPS_EN[] = {
    "Walk around the car and check that the front position lights, brake lights, puddle lamps and "
    "mirror heaters all work.",
    "If everything works, clear these history codes (with approval) and scan again after a few drives. "
    "Any code that comes back is a real issue.",
    "Battery at 12.2 V is fine for reading. Before changing any settings, connect a battery charger or "
    "power supply so it stays above 12.5 V.",
};
static const char *STEPS_AR[] = {
    "لف حول السيارة وتأكد أن إضاءة الموقع الأمامية، لمبات الفرامل، إضاءة الأرضية، وسخانات المرايا كلها تعمل.",
    "إذا كان كل شيء يعمل، نمسح هذه الأعطال القديمة (بعد الموافقة) ونعيد الفحص بعد عدة مشاوير. "
    "أي كود يرجع يعني مشكلة حقيقية.",
    "البطارية على 12.2 V مناسبة للقراءة فقط. قبل تغيير أي إعداد، نوصل شاحن بطارية حتى يبقى الجهد فوق 12.5 V.",
};

void carscan_sample(CarScan *s)
{
    memset(s, 0, sizeof *s);
    s->when = (time_t)1790680680;          /* 29 Sep 2026, 11:18 UTC */
    SET(s->adapter, "OBDLink EX r2.7.1");
    SET(s->protocol, "ISO 15765-4 CAN 11/500");
    s->ignition_on = 1;
    s->engine_running = 0;
    s->demo = 0;

    SET(s->vin, "6FPPXXMJ2SF000001");
    s->model_year = 2025;
    SET(s->make_en, "Ford");
    SET(s->make_ar, "فورد");
    SET(s->make_model_en, "Ford Ranger Raptor");
    SET(s->make_model_ar, "فورد رينجر رابتر");
    SET(s->engine_en, "3.0L V6 EcoBoost");
    SET(s->engine_ar, "محرك V6 سعة 3.0 لتر");
    s->battery_v = 12.2;
    s->odometer_km = 764;
    const char *odo_mods[] = {"VDM", "IPMA", "AWD"};       /* the 3 modules that keep the mileage */
    for (int i = 0; i < 3; i++) { SET(s->odo[i].module, odo_mods[i]); s->odo[i].km = 764.0; }
    s->odo_count = 3;
    s->odo_agree = 1;

    s->mil_on = 0;
    s->readiness_total = 0;                 /* engine off: readiness tests were not read */
    s->readiness_ready = 0;

    s->module_count = (int)(sizeof MODULES / sizeof MODULES[0]);
    for (int i = 0; i < s->module_count; i++) {
        ScanModule *m = &s->modules[i];
        SET(m->code, MODULES[i].code);
        SET(m->name_en, MODULES[i].en);
        SET(m->name_ar, MODULES[i].ar);
        m->can_id = MODULES[i].id;
        m->answered = 1;
    }
    s->modules_answered = s->module_count;

    s->dtc_count = (int)(sizeof DTCS / sizeof DTCS[0]);
    for (int i = 0; i < s->dtc_count; i++) {
        ScanDtc *d = &s->dtcs[i];
        SET(d->module, DTCS[i].mod);
        SET(d->code, DTCS[i].code);
        d->failure_type = 0x11;             /* "circuit short to ground" style sub-code */
        d->status = 0x08;                   /* confirmed earlier, not failing now */
        d->state = DTC_HISTORY;
        SET(d->group_en, DTCS[i].gen);
        SET(d->group_ar, DTCS[i].gar);
        SET(d->meaning_en, DTCS[i].en);
        SET(d->meaning_ar, DTCS[i].ar);
    }

    s->healthy = 1;
    SET(s->headline_en, "Healthy. No active faults.");
    SET(s->headline_ar, "السيارة سليمة. لا توجد أعطال حالية.");
    SET(s->summary_en, "All 28 modules answered. There are 11 old (history) codes, but none of them is "
                       "happening now, and the check-engine light is off for all of them. The engine, "
                       "gearbox, brakes, airbags and steering have no codes at all.");
    SET(s->summary_ar, "استجابت جميع الوحدات الإلكترونية (28 وحدة). توجد 11 رسالة خطأ قديمة، لكن لا يوجد "
                       "أي منها الآن، ولمبة فحص المحرك مطفأة لجميعها. المحرك وناقل الحركة والفرامل "
                       "والوسائد الهوائية والمقود بلا أي أعطال.");
    SET(s->cause_en, "Most likely cause: a one-time event, such as the battery being disconnected or run "
                     "low during shipping or dealer prep. Both sides (left and right) logged the same "
                     "fault at once, which points to a power event rather than broken bulbs or wiring.");
    SET(s->cause_ar, "السبب الأرجح: حدث لمرة واحدة، مثل فصل البطارية أو ضعفها أثناء الشحن أو تجهيز "
                     "الوكالة. الجهتان (اليمين واليسار) سجلتا نفس العطل في نفس الوقت، وهذا يدل على "
                     "مشكلة كهرباء مؤقتة وليس لمبات أو أسلاك تالفة.");
    s->step_count = 3;
    for (int i = 0; i < 3; i++) { SET(s->steps_en[i], STEPS_EN[i]); SET(s->steps_ar[i], STEPS_AR[i]); }

    s->wish_count = (int)(sizeof WISH / sizeof WISH[0]);
    for (int i = 0; i < s->wish_count; i++) {
        ScanWish *w = &s->wish[i];
        SET(w->feature_en, WISH[i].fen);
        SET(w->feature_ar, WISH[i].far);
        SET(w->module_en, WISH[i].men);
        SET(w->module_ar, WISH[i].mar);
        w->outlook = WISH[i].outlook;
    }
}
