/* eir.c - reads what a device tells us while we scan, and decides what
 * kind of device it is (keyboard, mouse, phone...).
 *
 * The data ("EIR" for classic devices, "advertising data" for Low Energy) is
 * a chain of records:   [length][type][data ... (length-1 bytes)]
 * A length of 0 ends the chain early. */
#include <string.h>
#include "bt.h"

#define EIR_FLAGS          0x01
#define EIR_UUID16_SOME    0x02
#define EIR_UUID16_ALL     0x03
#define EIR_NAME_SHORT     0x08
#define EIR_NAME_COMPLETE  0x09
#define EIR_TX_POWER       0x0A
#define EIR_CLASS_OF_DEV   0x0D
#define EIR_SERVICE_DATA16 0x16
#define EIR_APPEARANCE     0x19
#define EIR_MANUFACTURER   0xFF

/* remember a 16-bit service number once (at most 8) */
static void add_uuid(struct eir_info *out, uint16_t u)
{
    int i;
    for (i = 0; i < out->n_uuid; i++) if (out->uuid[i] == u) return;
    if (out->n_uuid < 8) out->uuid[out->n_uuid++] = u;
}

int eir_parse(const uint8_t *eir, size_t len, struct eir_info *out)
{
    size_t pos = 0;
    memset(out, 0, sizeof(*out));
    while (pos < len) {
        size_t field_len = eir[pos];
        const uint8_t *data;
        size_t dlen, i;
        uint8_t type;
        if (field_len == 0)            /* early end: the rest is padding */
            break;
        if (pos + 1 + field_len > len) /* record runs past the end: damaged */
            return -1;
        type = eir[pos + 1];
        data = eir + pos + 2;
        dlen = field_len - 1;
        switch (type) {
        case EIR_FLAGS:
            if (dlen >= 1) { out->has_flags = 1; out->flags = data[0]; }
            break;
        case EIR_NAME_SHORT:
        case EIR_NAME_COMPLETE:
            /* a complete name wins over a shortened one */
            if (!out->name_complete || type == EIR_NAME_COMPLETE) {
                clean_name((const char *)data, dlen, out->name, sizeof(out->name));
                out->has_name = out->name[0] != 0;
                out->name_complete = (type == EIR_NAME_COMPLETE);
            }
            break;
        case EIR_CLASS_OF_DEV:
            if (dlen >= 3) {
                out->has_class = 1;
                out->cod = data[0] | data[1] << 8 | (uint32_t)data[2] << 16;
            }
            break;
        case EIR_APPEARANCE:
            if (dlen >= 2) { out->has_appearance = 1; out->appearance = get_le16(data); }
            break;
        case EIR_UUID16_SOME:
        case EIR_UUID16_ALL:
            for (i = 0; i + 1 < dlen; i += 2) {
                uint16_t u = get_le16(data + i);
                if (u == 0x1812 || u == 0x1124)   /* HID over GATT / classic HID */
                    out->has_hid_uuid = 1;
                add_uuid(out, u);
            }
            break;
        case EIR_SERVICE_DATA16:              /* data for one service: [service 2 bytes][data] */
            if (dlen >= 2) add_uuid(out, get_le16(data));
            break;
        case EIR_TX_POWER:
            if (dlen >= 1) { out->has_tx = 1; out->tx_power = (int8_t)data[0]; }
            break;
        case EIR_MANUFACTURER:                /* [company 2 bytes][the maker's own bytes] */
            if (dlen >= 2 && !out->has_mfr) {
                out->has_mfr = 1;
                out->company = get_le16(data);
                out->mfr_len = (uint8_t)(dlen - 2 < sizeof(out->mfr) ? dlen - 2 : sizeof(out->mfr));
                memcpy(out->mfr, data + 2, out->mfr_len);
            }
            break;
        }
        pos += 1 + field_len;
    }
    return 0;
}

/* Classic "class of device": 24 bits.
 *   bits 12..8 = major class (5 = peripheral: keyboards, mice, gamepads)
 *   bits  7..2 = minor class; for peripherals:
 *       bit 6 = keyboard, bit 7 = pointing device (mouse),
 *       bits 5..2 = 1 joystick, 2 gamepad, 3 remote control, 5 tablet */
const char *kind_from_class(uint32_t cod)
{
    unsigned major = (cod >> 8) & 0x1f;
    unsigned minor = (cod >> 2) & 0x3f;
    switch (major) {
    case 1: return "computer";
    case 2: return "phone";
    case 3: return "network";
    case 4: return "audio";
    case 5: {
        int kb = (minor & 0x10) != 0, pt = (minor & 0x20) != 0;
        if (kb && pt) return "keyboard+mouse";
        if (kb) return "keyboard";
        if (pt) return "mouse";
        switch (minor & 0x0f) {
        case 1: return "joystick";
        case 2: return "gamepad";
        case 3: return "remote";
        case 5: return "tablet";
        }
        return "input";
    }
    case 6: return "imaging";
    case 7: return "wearable";
    case 8: return "toy";
    case 9: return "health";
    }
    return "other";
}

int class_is_hid(uint32_t cod)
{
    return ((cod >> 8) & 0x1f) == 5;
}

/* Low Energy "appearance": 16 bits = category (top 10 bits) + subcategory (low 6).
 * Category 15 = human interface device; its subcategories are below. */
const char *kind_from_appearance(uint16_t ap)
{
    unsigned cat = ap >> 6, sub = ap & 0x3f;
    switch (cat) {
    case 1: return "phone";
    case 2: return "computer";
    case 3: return "watch";
    case 15:
        switch (sub) {
        case 1: return "keyboard";
        case 2: return "mouse";
        case 3: return "joystick";
        case 4: return "gamepad";
        case 5: return "tablet";
        case 7: return "pen";
        }
        return "input";
    }
    return "other";
}

int appearance_is_hid(uint16_t ap)
{
    return (ap >> 6) == 15;
}
