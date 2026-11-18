/* fordscan.c - Ford's module names and addresses (see fordscan.h). The scan
 * itself (any car, read only) is carscan.c; it uses this table only on a Ford.
 *
 * THE FORD MODULE TABLE - where it comes from:
 *  [1] "Common Settings ... Module Address" F-150 2011-2018 FORScan spreadsheet,
 *      module listing (pickuptrucks.de attachment FORScan2011-2018.pdf):
 *      701 GPSM, 706 IPMA, 720 IPC, 724 SCCM, 726 BCM, 727 ACM, 730 PSCM,
 *      733 HVAC, 736 PAM, 737 RCM, 740 DDM, 741 PDM, 744 DSM, 751 RTM, 760 ABS,
 *      764 CCM, 783 DSP, 7A7 FCIM, 7B1 IPMB, 7B2 HUD, 7C4 SODL, 7C6 SODR,
 *      7D0 APIM, 7D5 DACMC.
 *  [2] FORScan forum, "Ford As Built Data Deciphering" (t=2012, module list by
 *      Kingsammy, 28 Apr 2017): 7E0 PCM, 7E1 TCM, 716 GWM, 734 HCM, 721 VDM,
 *      761 TCCM, 775 LTM (and the same values as [1] for the others).
 *  [3] Owner-forum FORScan lists for 2021+ Bronco / Ranger / Maverick (search
 *      results only, not one document I could read): 754 TCU, 725 WACM,
 *      6F0 BCMC, 732 GSM, 792 ATCM, 703 AWD.  -> marked sure = 0 below.
 *  Request = the address, answer = address + 8 (ISO 15765-4 11-bit physical
 *  addressing as Ford uses it; e.g. PCM 7E0 -> 7E8). "ms" = the module sat on
 *  MS-CAN in 2011-2019 Fords (on the 2023+ Ranger the gateway GWM forwards
 *  everything to the OBD port's HS-CAN pins).
 *
 * Odometer: Ford's "global" DID DD01 (total distance, 3 bytes, km) is answered
 * by several modules; reading it from each is the mileage check. The exact
 * resolution is not in a public Ford document I could read - 1 km per count is
 * what owner forums report (uncertain, see the report notes). */
#include <stddef.h>
#include "fordscan.h"

static const FordModule mods[] = {
    { "PCM",   0x7E0, 0, 1, "Powertrain (engine) control", "كمبيوتر المحرك" },
    { "TCM",   0x7E1, 0, 1, "Gearbox control", "كمبيوتر ناقل الحركة" },
    { "ABS",   0x760, 0, 1, "Anti-lock brakes", "نظام منع انغلاق الفرامل" },
    { "PSCM",  0x730, 0, 1, "Power steering", "المقود الكهربائي" },
    { "VDM",   0x721, 0, 0, "Vehicle dynamics", "ثبات السيارة" },
    { "AWD",   0x703, 0, 0, "All-wheel drive", "الدفع الرباعي" },
    { "ATCM",  0x792, 0, 0, "All-terrain control", "أنماط القيادة للطرق الوعرة" },
    { "GSM",   0x732, 0, 0, "Gear shift", "ذراع القير" },
    { "CCM",   0x764, 0, 1, "Cruise control (radar)", "مثبت السرعة (رادار)" },
    { "IPMA",  0x706, 0, 1, "Front camera", "الكاميرا الأمامية" },
    { "SODL",  0x7C4, 1, 1, "Blind spot, left", "النقطة العمياء، يسار" },
    { "SODR",  0x7C6, 1, 1, "Blind spot, right", "النقطة العمياء، يمين" },
    { "RCM",   0x737, 0, 1, "Airbags", "الوسائد الهوائية" },
    { "BdyCM", 0x726, 1, 1, "Body control", "التحكم بالهيكل" },
    { "BCMC",  0x6F0, 0, 0, "Body control C", "التحكم بالهيكل C" },
    { "HCM",   0x734, 0, 1, "Headlamps", "المصابيح الأمامية" },
    { "DDM",   0x740, 1, 1, "Driver door", "باب السائق" },
    { "PDM",   0x741, 1, 1, "Passenger door", "باب الراكب" },
    { "HVAC",  0x733, 1, 1, "Air conditioning", "المكيف" },
    { "IPC",   0x720, 1, 1, "Instrument cluster", "العدادات" },
    { "SCCM",  0x724, 1, 1, "Steering column switches", "مفاتيح عمود المقود" },
    { "FCIM",  0x7A7, 1, 1, "Front controls", "أزرار التحكم الأمامية" },
    { "APIM",  0x7D0, 1, 1, "SYNC screen", "شاشة SYNC" },
    { "ACM",   0x727, 1, 1, "Audio control", "التحكم بالصوت" },
    { "DSP",   0x783, 1, 1, "Audio amplifier", "مضخم الصوت" },
    { "TCU",   0x754, 0, 0, "Modem / telematics", "وحدة الاتصال (مودم)" },
    { "RTM",   0x751, 1, 1, "Key-fob receiver", "مستقبل الريموت" },
    { "WACM",  0x725, 0, 0, "Wireless phone charger", "الشاحن اللاسلكي" },
    { "GWM",   0x716, 0, 1, "Gateway", "البوابة (Gateway)" },
    { "PAM",   0x736, 1, 1, "Parking sensors", "حساسات الركن" },
    { "TCCM",  0x761, 0, 1, "Transfer case", "علبة التحويل" },
    { "IPMB",  0x7B1, 1, 1, "Rear camera", "كاميرا الرجوع" },
    { "HUD",   0x7B2, 1, 1, "Head-up display", "شاشة العرض على الزجاج" },
    { "DSM",   0x744, 1, 1, "Driver seat", "مقعد السائق" },
    { "LTM",   0x775, 1, 1, "Tailgate / liftgate", "الباب الخلفي" },
    { "GPSM",  0x701, 1, 1, "GPS receiver", "مستقبل GPS" },
    { "DACMC", 0x7D5, 1, 1, "Digital audio control", "التحكم بالصوت الرقمي" },
};
#define MOD_COUNT ((int)(sizeof mods / sizeof mods[0]))

int fordscan_module_count(void) { return MOD_COUNT; }
const FordModule *fordscan_module(int i) { return i >= 0 && i < MOD_COUNT ? &mods[i] : NULL; }

const FordModule *fordscan_by_id(int id)
{
    for (int i = 0; i < MOD_COUNT; i++) if (mods[i].id == id) return &mods[i];
    return NULL;
}
