/* dtc_db.c - fault code meanings in English and Arabic (see dtc_db.h).
 *
 * Sources:
 *  - P/C/U codes with no module: the generic SAE J2012 meanings, in plain words.
 *  - The 11 Ford body/network codes: a real FORScan "Raptor Health Scan"
 *    report of 29 Sep 2026 (English and Arabic wording taken from it).
 *  - Common Ford "lost communication" U-codes and battery codes: SAE J2012
 *    network code list (U0100-U0299 = lost communication with module X).
 * Arabic: Modern Standard Arabic with the words Gulf drivers use
 * (القير for the gear lever, دبة التلوث for the catalytic converter).
 *
 * Any car: a code like P0133 means the same on every make ("generic"), but a
 * maker's own codes (P1..., B1..., U1...) mean something different on each
 * make. The entries with a module name, and the maker-specific codes, are
 * Ford's: dtc_lookup_car() leaves them out for every other make. */
#include <string.h>
#include "dtc_db.h"

static const DtcInfo db[] = {
    /* ---- from his report (module-specific wording) ---- */
    { "BdyCM", "B10F3", DG_LIGHTS, "Left front position light: open or short circuit",
      "إضاءة الموقع الأمامية اليسرى: انقطاع أو تماس في الدائرة", "B10F4", "front position lights", "إضاءة الموقع الأمامية" },
    { "BdyCM", "B10F4", DG_LIGHTS, "Right front position light: open or short circuit",
      "إضاءة الموقع الأمامية اليمنى: انقطاع أو تماس في الدائرة", "B10F3", "front position lights", "إضاءة الموقع الأمامية" },
    { "BdyCM", "B1444", DG_LIGHTS, "Left rear stop/position lamp: open or short circuit",
      "لمبة الفرامل/الموقع الخلفية اليسرى: انقطاع أو تماس", "B1448", "brake lights", "لمبات الفرامل" },
    { "BdyCM", "B1448", DG_LIGHTS, "Right rear stop/position lamp: open or short circuit",
      "لمبة الفرامل/الموقع الخلفية اليمنى: انقطاع أو تماس", "B1444", "brake lights", "لمبات الفرامل" },
    { "DDM", "B1163", DG_MIRRORS, "Left mirror heater: open or short circuit",
      "سخان المرآة اليسرى: انقطاع أو تماس", "B1164", "mirror heaters", "سخانات المرايا" },
    { "DDM", "B1165", DG_MIRRORS, "Left puddle lamp (under mirror): open or short circuit",
      "إضاءة الأرضية تحت المرآة اليسرى: انقطاع أو تماس", "B1166", "puddle lamps", "إضاءة الأرضية" },
    { "PDM", "B1164", DG_MIRRORS, "Right mirror heater: open or short circuit",
      "سخان المرآة اليمنى: انقطاع أو تماس", "B1163", "mirror heaters", "سخانات المرايا" },
    { "PDM", "B1166", DG_MIRRORS, "Right puddle lamp (under mirror): open or short circuit",
      "إضاءة الأرضية تحت المرآة اليمنى: انقطاع أو تماس", "B1165", "puddle lamps", "إضاءة الأرضية" },
    { "HCM", "U0122", DG_NETWORK, "Headlamp module lost contact with the stability-control module",
      "وحدة المصابيح الأمامية فقدت الاتصال بوحدة التحكم بالثبات", NULL, NULL, NULL },
    { "HVAC", "U0401", DG_NETWORK, "A/C module got invalid data from the engine computer",
      "وحدة المكيف استلمت بيانات غير صحيحة من كمبيوتر المحرك", NULL, NULL, NULL },
    { "ACM", "B150C", DG_NETWORK, "Audio system: a speaker-network message went missing",
      "نظام الصوت: رسالة مفقودة في شبكة السماعات", NULL, NULL, NULL },

    /* ---- generic engine codes (SAE J2012), any module ---- */
    { NULL, "P0101", DG_ENGINE, "Air flow sensor (MAF): readings out of range", "حساس تدفق الهواء (MAF): القراءات خارج النطاق", NULL, NULL, NULL },
    { NULL, "P0102", DG_ENGINE, "Air flow sensor (MAF): signal too low", "حساس تدفق الهواء (MAF): الإشارة منخفضة جدًا", NULL, NULL, NULL },
    { NULL, "P0113", DG_ENGINE, "Intake air temperature sensor: signal too high", "حساس حرارة الهواء الداخل: الإشارة مرتفعة جدًا", NULL, NULL, NULL },
    { NULL, "P0128", DG_ENGINE, "Engine warms up too slowly (often the thermostat)", "المحرك يسخن ببطء (غالبًا الثرموستات)", NULL, NULL, NULL },
    { NULL, "P0133", DG_ENGINE, "Oxygen sensor (bank 1, sensor 1) responds slowly", "حساس الأكسجين (الصف 1، الحساس 1) بطيء الاستجابة", NULL, NULL, NULL },
    { NULL, "P0135", DG_ENGINE, "Oxygen sensor heater (bank 1, sensor 1) circuit", "سخان حساس الأكسجين (الصف 1، الحساس 1): عطل في الدائرة", NULL, NULL, NULL },
    { NULL, "P0141", DG_ENGINE, "Oxygen sensor heater (bank 1, sensor 2) circuit", "سخان حساس الأكسجين (الصف 1، الحساس 2): عطل في الدائرة", NULL, NULL, NULL },
    { NULL, "P0171", DG_ENGINE, "Fuel mixture too lean (bank 1)", "خليط الوقود فقير جدًا (الصف 1)", NULL, NULL, NULL },
    { NULL, "P0172", DG_ENGINE, "Fuel mixture too rich (bank 1)", "خليط الوقود غني جدًا (الصف 1)", NULL, NULL, NULL },
    { NULL, "P0174", DG_ENGINE, "Fuel mixture too lean (bank 2)", "خليط الوقود فقير جدًا (الصف 2)", NULL, NULL, NULL },
    { NULL, "P0175", DG_ENGINE, "Fuel mixture too rich (bank 2)", "خليط الوقود غني جدًا (الصف 2)", NULL, NULL, NULL },
    { NULL, "P0217", DG_ENGINE, "Engine overheating", "ارتفاع حرارة المحرك", NULL, NULL, NULL },
    { NULL, "P0300", DG_ENGINE, "Random misfires (more than one cylinder)", "خلل عشوائي في الاشتعال (في أكثر من سلندر)", NULL, NULL, NULL },
    { NULL, "P0301", DG_ENGINE, "Misfire in cylinder 1", "خلل في الاشتعال في السلندر 1", NULL, NULL, NULL },
    { NULL, "P0302", DG_ENGINE, "Misfire in cylinder 2", "خلل في الاشتعال في السلندر 2", NULL, NULL, NULL },
    { NULL, "P0303", DG_ENGINE, "Misfire in cylinder 3", "خلل في الاشتعال في السلندر 3", NULL, NULL, NULL },
    { NULL, "P0304", DG_ENGINE, "Misfire in cylinder 4", "خلل في الاشتعال في السلندر 4", NULL, NULL, NULL },
    { NULL, "P0305", DG_ENGINE, "Misfire in cylinder 5", "خلل في الاشتعال في السلندر 5", NULL, NULL, NULL },
    { NULL, "P0306", DG_ENGINE, "Misfire in cylinder 6", "خلل في الاشتعال في السلندر 6", NULL, NULL, NULL },
    { NULL, "P0335", DG_ENGINE, "Crankshaft position sensor circuit", "حساس موضع عمود الكرنك: عطل في الدائرة", NULL, NULL, NULL },
    { NULL, "P0340", DG_ENGINE, "Camshaft position sensor circuit", "حساس موضع عمود الكامات: عطل في الدائرة", NULL, NULL, NULL },
    { NULL, "P0401", DG_ENGINE, "Exhaust gas recirculation (EGR): too little flow", "نظام إعادة تدوير غازات العادم (EGR): التدفق قليل", NULL, NULL, NULL },
    { NULL, "P0420", DG_ENGINE, "Catalytic converter (bank 1) not working well enough", "دبة التلوث (المحول الحفاز) في الصف 1 لا تعمل بكفاءة كافية", NULL, NULL, NULL },
    { NULL, "P0430", DG_ENGINE, "Catalytic converter (bank 2) not working well enough", "دبة التلوث (المحول الحفاز) في الصف 2 لا تعمل بكفاءة كافية", NULL, NULL, NULL },
    { NULL, "P0440", DG_ENGINE, "Fuel vapour (EVAP) system fault", "نظام أبخرة الوقود (EVAP): عطل", NULL, NULL, NULL },
    { NULL, "P0442", DG_ENGINE, "Fuel vapour (EVAP) system: small leak (check the fuel cap)", "نظام أبخرة الوقود (EVAP): تسريب صغير (افحص غطاء خزان الوقود)", NULL, NULL, NULL },
    { NULL, "P0455", DG_ENGINE, "Fuel vapour (EVAP) system: large leak (check the fuel cap)", "نظام أبخرة الوقود (EVAP): تسريب كبير (افحص غطاء خزان الوقود)", NULL, NULL, NULL },
    { NULL, "P0456", DG_ENGINE, "Fuel vapour (EVAP) system: very small leak", "نظام أبخرة الوقود (EVAP): تسريب صغير جدًا", NULL, NULL, NULL },
    { NULL, "P0500", DG_ENGINE, "Vehicle speed sensor", "حساس سرعة السيارة", NULL, NULL, NULL },
    { NULL, "P0505", DG_ENGINE, "Idle speed control system", "نظام التحكم بسرعة المحرك على الخمول (السلانسي)", NULL, NULL, NULL },
    { NULL, "P0507", DG_ENGINE, "Idle speed higher than expected", "سرعة المحرك على الخمول أعلى من المتوقع", NULL, NULL, NULL },
    { NULL, "P0562", DG_POWER, "System voltage low (battery / alternator)", "جهد الكهرباء منخفض (البطارية / الدينمو)", NULL, NULL, NULL },
    { NULL, "P0700", DG_GEARBOX, "Transmission control system: the gearbox computer stored a fault", "نظام التحكم بناقل الحركة: كمبيوتر القير سجّل عطلًا", NULL, NULL, NULL },

    /* ---- wheel speed sensors (SAE J2012 chassis codes) ---- */
    { NULL, "C0035", DG_BRAKES, "Left front wheel speed sensor", "حساس سرعة العجلة الأمامية اليسرى", "C0040", NULL, NULL },
    { NULL, "C0040", DG_BRAKES, "Right front wheel speed sensor", "حساس سرعة العجلة الأمامية اليمنى", "C0035", NULL, NULL },
    { NULL, "C0045", DG_BRAKES, "Left rear wheel speed sensor", "حساس سرعة العجلة الخلفية اليسرى", "C0050", NULL, NULL },
    { NULL, "C0050", DG_BRAKES, "Right rear wheel speed sensor", "حساس سرعة العجلة الخلفية اليمنى", "C0045", NULL, NULL },

    /* ---- network codes: "lost communication with ..." (SAE J2012), common on Fords ---- */
    { NULL, "U0100", DG_NETWORK, "Lost contact with the engine computer (PCM)", "فقدان الاتصال بكمبيوتر المحرك (PCM)", NULL, NULL, NULL },
    { NULL, "U0101", DG_NETWORK, "Lost contact with the gearbox computer (TCM)", "فقدان الاتصال بكمبيوتر ناقل الحركة (TCM)", NULL, NULL, NULL },
    { NULL, "U0121", DG_NETWORK, "Lost contact with the anti-lock brakes (ABS)", "فقدان الاتصال بوحدة منع انغلاق الفرامل (ABS)", NULL, NULL, NULL },
    { NULL, "U0122", DG_NETWORK, "Lost contact with the stability-control module", "فقدان الاتصال بوحدة التحكم بالثبات", NULL, NULL, NULL },
    { NULL, "U0131", DG_NETWORK, "Lost contact with the power steering (PSCM)", "فقدان الاتصال بوحدة المقود الكهربائي (PSCM)", NULL, NULL, NULL },
    { NULL, "U0140", DG_NETWORK, "Lost contact with the body control module (BCM)", "فقدان الاتصال بوحدة التحكم بالهيكل (BCM)", NULL, NULL, NULL },
    { NULL, "U0151", DG_NETWORK, "Lost contact with the airbag module (RCM)", "فقدان الاتصال بوحدة الوسائد الهوائية (RCM)", NULL, NULL, NULL },
    { NULL, "U0155", DG_NETWORK, "Lost contact with the instrument cluster (IPC)", "فقدان الاتصال بالعدادات (IPC)", NULL, NULL, NULL },
    { NULL, "U0164", DG_NETWORK, "Lost contact with the air conditioning (HVAC)", "فقدان الاتصال بوحدة المكيف (HVAC)", NULL, NULL, NULL },
    { NULL, "U0184", DG_NETWORK, "Lost contact with the radio (ACM)", "فقدان الاتصال بالراديو (ACM)", NULL, NULL, NULL },
    { NULL, "U0198", DG_NETWORK, "Lost contact with the telematics modem (TCU)", "فقدان الاتصال بوحدة الاتصال (TCU)", NULL, NULL, NULL },
    { NULL, "U0212", DG_NETWORK, "Lost contact with the steering column switches (SCCM)", "فقدان الاتصال بمفاتيح عمود المقود (SCCM)", NULL, NULL, NULL },
    { NULL, "U0214", DG_NETWORK, "Lost contact with the remote function module", "فقدان الاتصال بوحدة التحكم عن بعد", NULL, NULL, NULL },
    { NULL, "U0253", DG_NETWORK, "Lost contact with the screen / infotainment module (APIM)", "فقدان الاتصال بوحدة الشاشة والترفيه (APIM)", NULL, NULL, NULL },
    { NULL, "U0256", DG_NETWORK, "Lost contact with the front controls (FCIM)", "فقدان الاتصال بأزرار التحكم الأمامية (FCIM)", NULL, NULL, NULL },
    { NULL, "U0401", DG_NETWORK, "Invalid data received from the engine computer (PCM)", "بيانات غير صحيحة من كمبيوتر المحرك (PCM)", NULL, NULL, NULL },
    { NULL, "U3000", DG_NETWORK, "Fault inside the module itself", "عطل داخلي في الوحدة نفسها", NULL, NULL, NULL },
    { NULL, "U3003", DG_POWER, "Battery voltage out of range (too low or too high)", "جهد البطارية خارج النطاق (منخفض أو مرتفع)", NULL, NULL, NULL },

    /* ---- common Ford body codes (Ford-specific B-codes) ---- */
    { NULL, "B1318", DG_POWER, "Battery voltage low", "جهد البطارية منخفض", NULL, NULL, NULL },
    { NULL, "B1342", DG_BODY, "Fault inside the module (it may need checking or replacing)", "عطل داخلي في الوحدة (قد تحتاج فحصًا أو تبديلًا)", NULL, NULL, NULL },
};

/* SAE J2012: generic = P0, P2, P34-P39, B0, C0, U0 (the same meaning on every car) */
int dtc_is_generic(const char *code)
{
    if (!code || strlen(code) < 5) return 0;
    if (code[1] == '0') return 1;
    if (code[0] == 'P' && code[1] == '2') return 1;
    if (code[0] == 'P' && code[1] == '3' && code[2] >= '4' && code[2] <= '9') return 1;
    return 0;
}

const DtcInfo *dtc_lookup_car(const char *module, const char *code, int ford)
{
    if (ford) return dtc_lookup(module, code);
    if (!dtc_is_generic(code)) return NULL;               /* a maker's own code: we only know Ford's */
    for (size_t i = 0; i < sizeof db / sizeof db[0]; i++)
        if (!db[i].module && !strcmp(db[i].code, code)) return &db[i];   /* only the "any module" entries */
    return NULL;
}

const DtcInfo *dtc_lookup(const char *module, const char *code)
{
    const DtcInfo *any = NULL;
    for (size_t i = 0; i < sizeof db / sizeof db[0]; i++) {
        if (strcmp(db[i].code, code)) continue;
        if (db[i].module && module && !strcmp(db[i].module, module)) return &db[i];   /* exact module: best */
        if (!db[i].module) any = &db[i];
        else if (!any) any = &db[i];        /* the same code from another module: still a good hint */
    }
    return any;
}

static const char *const groups[DG_COUNT][2] = {
    [DG_LIGHTS]   = { "Exterior lights · Body Control Module", "الإضاءة الخارجية · وحدة التحكم بالهيكل" },
    [DG_MIRRORS]  = { "Mirrors · Door modules", "المرايا · وحدات الأبواب" },
    [DG_NETWORK]  = { "Communication between modules", "الاتصال بين الوحدات" },
    [DG_ENGINE]   = { "Engine and emissions", "المحرك والانبعاثات" },
    [DG_GEARBOX]  = { "Gearbox", "ناقل الحركة" },
    [DG_BRAKES]   = { "Brakes, stability and wheels", "الفرامل والثبات والعجلات" },
    [DG_STEERING] = { "Steering", "المقود" },
    [DG_AIRBAGS]  = { "Airbags and seatbelts", "الوسائد الهوائية وأحزمة الأمان" },
    [DG_BODY]     = { "Body and comfort", "الهيكل ووسائل الراحة" },
    [DG_POWER]    = { "Battery and charging", "البطارية والشحن" },
    [DG_AUDIO]    = { "Audio and screen", "الصوت والشاشة" },
    [DG_CLIMATE]  = { "Air conditioning", "المكيف" },
};

const char *dtc_group_name(int group, const char *lang)
{
    if (group < 0 || group >= DG_COUNT) group = DG_BODY;
    return groups[group][lang && !strcmp(lang, "ar")];
}

int dtc_group_of(const char *module, const char *code, int ford)
{
    const DtcInfo *d = dtc_lookup_car(module, code, ford);
    if (d) return d->group;
    if (module) {                                    /* no entry: guess from the module (Ford's names, and TCM) */
        if (!strcmp(module, "RCM") || !strcmp(module, "OCS")) return DG_AIRBAGS;
        if (!strcmp(module, "PSCM")) return DG_STEERING;
        if (!strcmp(module, "ABS") || !strcmp(module, "VDM")) return DG_BRAKES;
        if (!strcmp(module, "TCM") || !strcmp(module, "GSM")) return DG_GEARBOX;
        if (!strcmp(module, "HVAC")) return DG_CLIMATE;
        if (!strcmp(module, "ACM") || !strcmp(module, "APIM") || !strcmp(module, "DSP")) return DG_AUDIO;
    }
    switch (code[0]) {                               /* else from the letter */
    case 'P': return DG_ENGINE;
    case 'C': return DG_BRAKES;
    case 'U': return DG_NETWORK;
    default:  return DG_BODY;
    }
}

/* ISO 14229 / SAE J2012 "failure type" byte: the most common ones */
static const struct { unsigned char type; const char *en, *ar; } failures[] = {
    { 0x01, "general electrical fault", "عطل كهربائي عام" },
    { 0x11, "short to ground", "تماس مع الأرضي" },
    { 0x12, "short to battery", "تماس مع موجب البطارية" },
    { 0x13, "circuit open", "انقطاع في الدائرة" },
    { 0x16, "voltage too low", "الجهد منخفض" },
    { 0x17, "voltage too high", "الجهد مرتفع" },
    { 0x1C, "voltage out of range", "الجهد خارج النطاق" },
    { 0x49, "internal fault", "عطل داخلي" },
    { 0x55, "not set up (not configured)", "غير مبرمجة" },
    { 0x64, "signal does not make sense", "إشارة غير منطقية" },
    { 0x86, "invalid signal", "إشارة غير صالحة" },
    { 0x87, "missing message", "رسالة مفقودة" },
    { 0x92, "not working as expected", "الأداء غير سليم" },
};

const char *dtc_failure_text(unsigned char type, const char *lang)
{
    int ar = lang && !strcmp(lang, "ar");
    for (size_t i = 0; i < sizeof failures / sizeof failures[0]; i++)
        if (failures[i].type == type) return ar ? failures[i].ar : failures[i].en;
    return "";
}
