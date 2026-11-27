#!/bin/bash
# mutants.sh - proves the tests can FAIL: each "mutant" is a copy of nzeus-bt's
# source with ONE deliberate mistake. The unit + flow tests must catch every one
# (a mutant that passes all tests = a blind spot in the tests).
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
W=$(mktemp -d /tmp/nzbt-mutants.XXXXXX)
trap 'rm -rf "$W"' EXIT
SURVIVED=0; CAUGHT=0

# mutant FILE "description" "exact original text" "broken text"
mutant() {
    local file=$1 name=$2
    rm -rf $W/src && mkdir -p $W/src && cp "$SRC"/*.c "$SRC"/*.h "$HERE/unit_tests.c" $W/src/
    if ! python3 - "$W/src/$file" "$3" "$4" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit(1)
open(path, "w").write(s.replace(old, new))
PY
    then echo "  mutant '$name': text not found in $file - NOT APPLIED"; SURVIVED=$((SURVIVED+1)); return; fi
    (cd $W/src && gcc -O0 -w -DNZBT_FAKE -o $W/fake main.c util.c eir.c store.c mgmt.c sdp.c hid.c hogp.c io.c &&
                  gcc -O0 -w -I. -o $W/unit unit_tests.c util.c eir.c store.c sdp.c) 2> $W/cc.log
    if [ $? != 0 ]; then echo "  mutant '$name': does not compile (fix the mutant)"; SURVIVED=$((SURVIVED+1)); return; fi
    if ! timeout 60 $W/unit > $W/unit.log 2>&1; then
        echo "  caught by unit tests: $name"; CAUGHT=$((CAUGHT+1)); return
    fi
    if ! NZBT_STOP_ON_FAIL=1 timeout 300 python3 "$HERE/test_flows.py" $W/fake > $W/flows.log 2>&1; then
        echo "  caught by flow tests ($(grep -m1 FAIL $W/flows.log | sed 's/^ *FAIL //')): $name"
        CAUGHT=$((CAUGHT+1)); return
    fi
    echo "  MUTANT SURVIVED: $name"; SURVIVED=$((SURVIVED+1))
}

echo "=== nzeus-bt mutants"
mutant util.c  "address bytes not turned around" \
    'a->b[5 - i] = (uint8_t)(hi << 4 | lo);' 'a->b[i] = (uint8_t)(hi << 4 | lo);'
mutant eir.c   "keyboard and mouse bits swapped in class of device" \
    'int kb = (minor & 0x10) != 0, pt = (minor & 0x20) != 0;' 'int kb = (minor & 0x20) != 0, pt = (minor & 0x10) != 0;'
mutant eir.c   "shortened name beats the complete name" \
    'if (!out->name_complete || type == EIR_NAME_COMPLETE) {' 'if (1) {'
mutant eir.c   "damaged scan data accepted" \
    'if (pos + 1 + field_len > len) /* record runs past the end: damaged */
            return -1;' 'if (pos + 1 + field_len > len) /* record runs past the end: damaged */
            break;'
mutant sdp.c   "continuation token not sent back (only the first piece of a long answer)" \
    'if (cont_len == 0) return (int)total;          /* that was the last piece */' 'return (int)total;'
mutant sdp.c   "takes the first descriptor even if it is not a report descriptor (0x22)" \
    'kind == 0x22 &&' '1 &&'
mutant store.c "link key read back one byte short" \
    'd->has_linkkey = hex_decode(v, d->linkkey, 16) == 16;' 'd->has_linkkey = hex_decode(v, d->linkkey, 15) == 15;'
mutant store.c "LE key role (master) not saved" \
    'k->master = m; k->type = t;' 'k->master = 0; k->type = t;'
mutant main.c  "link keys sent to the kernel with a wrong size" \
    'st = req(MGMT_OP_LOAD_LINK_KEYS, buf, (uint16_t)(3 + 25 * count), NULL, NULL, 5000);' \
    'st = req(MGMT_OP_LOAD_LINK_KEYS, buf, (uint16_t)(3 + 24 * count), NULL, NULL, 5000);'
mutant main.c  "power on before the saved keys are loaded" \
    'n = store_list(&devs);
    load_keys(devs, n);' 'n = store_list(&devs);
    set_mode(MGMT_OP_SET_POWERED, 1);
    load_keys(devs, n);'
mutant main.c  "mouse pairing never confirmed" \
    'mgmt_send(&M, MGMT_OP_USER_CONFIRM_REPLY, (uint16_t)g_index, cp, 7);' '(void)cp;'
mutant main.c  "PIN printed differs from the PIN sent" \
    'memcpy(cp + 8, pin, strlen(pin));' 'memcpy(cp + 8, "000000", strlen(pin));'
mutant main.c  "daemon accepts devices that are not paired" \
    'if (store_load(&peer, &d) != 0 || is_le(&d)) {     /* only devices we paired */' \
    'if (store_load(&peer, &d) != 0 && 0) {     /* only devices we paired */'
mutant main.c  "classic devices not allowed to reconnect (ADD_DEVICE action 2 for all)" \
    'p[7] = is_le(d) ? 0x02 : 0x01;' 'p[7] = 0x02;'
mutant main.c  "keys from the daemon not saved (store_hint ignored the wrong way)" \
    'if (!p[0]) { dbg("link key not meant to be stored"); return 1; }' 'if (p[0]) { return 1; }'
mutant hid.c   "HID connect without encryption" \
    'ctrl = io_l2cap_connect(&d->addr, ADDR_BREDR, PSM_HID_CTRL, 0, BT_SEC_MEDIUM, timeout_ms);' \
    'ctrl = io_l2cap_connect(&d->addr, ADDR_BREDR, PSM_HID_CTRL, 0, BT_SEC_LOW, timeout_ms);'
mutant hid.c   "Bluetooth-SIG vendor flag never set" \
    'req.flags = 1u << HIDP_BLUETOOTH_VENDOR_ID;' 'req.flags = 0;'
mutant hogp.c  "LE report ID not put in front" \
    'if (rp->id) ev.u.input2.data[off++] = rp->id;   /* report ID goes first */' '(void)off;'
mutant hogp.c  "long report map: only the first piece read" \
    'if (n - 1 < a->mtu - 1 || total >= max) return total;   /* short piece = the end */' 'return total;'
mutant hogp.c  "notifications switched on with the wrong value" \
    'static const uint8_t on[2] = { 1, 0 };' 'static const uint8_t on[2] = { 2, 0 };'
mutant hogp.c  "device's own questions not answered" \
    '            else answer_device_request(&a, buf, n);
        }
        for (s = 0;' '            else (void)0;
        }
        for (s = 0;'
mutant main.c  "service keeps running when the dongle is unplugged" \
    'g_stop = 1;
        return;' 'return;'
mutant main.c  "data channels not bound to our controller" \
    'io_set_local_addr(&ci.addr);          /* our data channels start from this controller */' '(void)0;'
mutant eir.c   "survey: maker data keeps the company number as data" \
    'memcpy(out->mfr, data + 2, out->mfr_len);' 'memcpy(out->mfr, data, out->mfr_len);'
mutant eir.c   "survey: a long maker record overflows" \
    'out->mfr_len = (uint8_t)(dlen - 2 < sizeof(out->mfr) ? dlen - 2 : sizeof(out->mfr));' 'out->mfr_len = (uint8_t)(dlen - 2);'
mutant eir.c   "survey: transmit power read as a positive number" \
    'out->tx_power = (int8_t)data[0];' 'out->tx_power = (int8_t)(data[0] & 0x7f);'
mutant main.c  "survey: prints the scan's short lines" \
    'if (survey) { print_survey_line(f); continue; }' ';'
mutant mgmt.c  "events of other controllers not ignored" \
    'if (m->filter && index != m->index && index != MGMT_INDEX_NONE) {' 'if (0) {'
mutant mgmt.c  "reply matched without checking the opcode" \
    'get_le16(buf + 6) == g_wait.op && index == g_wait.index) {' 'index == g_wait.index) {'

echo "nzeus-bt mutants: $CAUGHT caught, $SURVIVED survived"
[ $SURVIVED = 0 ]
