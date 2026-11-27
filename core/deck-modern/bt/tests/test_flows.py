#!/usr/bin/env python3
"""test_flows.py - runs every nzeus-bt command against fake_kernel.py.

usage: test_flows.py PROGRAM [PREFIX...]
  PROGRAM = the -DNZBT_FAKE build of nzeus-bt
  PREFIX  = e.g. qemu-mipsel, to run the MIPS build on the PC
Prints one PASS/FAIL line per check; exit code 1 if anything failed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_kernel import (FakeKernel, Device, Gatt, hid_sdp_answer, pnp_sdp_answer,  # noqa: E402
                         EV_NEW_LINK_KEY, UHID_OUTPUT, UHID_GET_REPORT, addr_bytes)

PROG = sys.argv[1]
PREFIX = sys.argv[2:]
SLOW = 4 if PREFIX else 1          # qemu is slower: allow more time
FAILS = []
CHECKS = [0]
CHILDREN = []                      # background nzeus-bt daemons: always stopped at the end


def stop_children():
    for c in CHILDREN:
        if c.poll() is None:
            c.kill()


import atexit  # noqa: E402
atexit.register(stop_children)


def check(cond, what):
    CHECKS[0] += 1
    if cond:
        print("  PASS " + what)
    else:
        print("  FAIL " + what)
        FAILS.append(what)
        if os.environ.get("NZBT_STOP_ON_FAIL"):     # mutant runs: one failure is enough
            sys.stdout.flush()
            stop_children()
            os._exit(1)


def run(*args, stdin=None, timeout=60):
    p = subprocess.run(PREFIX + [PROG] + list(args), input=stdin, capture_output=True, text=True,
                       timeout=timeout * SLOW, env=ENV)
    if p.stderr.strip():
        print("     (stderr) " + p.stderr.strip().replace("\n", "\n     (stderr) ")[:2000])
    return p.returncode, p.stdout


def wait_for(fn, seconds=5.0):
    end = time.time() + seconds * SLOW
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.05)
    return fn()


def store_file(addr):
    return os.path.join(STORE, "devices", addr.replace(":", "-") + ".conf")


def store_fields(addr):
    try:
        text = open(store_file(addr)).read()
    except OSError:
        return None
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line and not line.startswith("#"))


# ---- the devices around the fake console ----
KB_DESC = bytes.fromhex(
    "05010906a101050719e029e71500250175019508810295017508810195057501"
    "0508190129059102950175039101950675081500256505071900296581 00c0".replace(" ", "")) + bytes.fromhex(
    "050c0901a1018503150025017501950b0ae2000ae9000aea000acd000ab5000ab6000a83010a8a010a92010a94010a2102810295057501810 3c0".replace(" ", ""))
MOUSE_DESC = bytes.fromhex(
    "05010902a1010901a10005091901290315002501950375018102950175058103"
    "0501093009310938158125 7f750895038106c0c0".replace(" ", ""))
REPORT_MAP = bytes.fromhex(
    "05010902a10185010901a10005091901290315002501950375018102950175058103"
    "0501093009310938158125 7f750895038106c0c0".replace(" ", "")) + bytes.fromhex(
    "05010906a101850205071 9e029e71500250175019508810295017508810195057501"
    "05081901290591029501750391019506750815002565050719002965810 0c0".replace(" ", ""))


def le_gatt():
    a = {
        1: (0x2800, b"\x00\x18"), 2: (0x2803, bytes([0x02, 3, 0, 0x00, 0x2A])), 3: (0x2A00, b"LE Mouse"),
        4: (0x2800, b"\x0a\x18"), 5: (0x2803, bytes([0x02, 6, 0, 0x50, 0x2A])),
        6: (0x2A50, bytes([2, 0x6d, 0x04, 0x34, 0x12, 0x01, 0x00])),
        7: (0x2800, b"\x12\x18"),
        8: (0x2803, bytes([0x02, 9, 0, 0x4A, 0x2A])), 9: (0x2A4A, bytes([0x11, 0x01, 0x21, 0x02])),
        10: (0x2803, bytes([0x02, 11, 0, 0x4B, 0x2A])), 11: (0x2A4B, REPORT_MAP),
        12: (0x2803, bytes([0x12, 13, 0, 0x4D, 0x2A])), 13: (0x2A4D, b"\0\0\0\0"),
        14: (0x2902, b"\0\0"), 15: (0x2908, b"\x01\x01"),
        16: (0x2803, bytes([0x0E, 17, 0, 0x4D, 0x2A])), 17: (0x2A4D, b"\0"), 18: (0x2908, b"\x02\x02"),
        19: (0x2803, bytes([0x12, 20, 0, 0x4D, 0x2A])), 20: (0x2A4D, b"\0" * 8),
        21: (0x2902, b"\0\0"), 22: (0x2908, b"\x02\x01"),
        23: (0x2800, b"\x0f\x18"), 24: (0x2803, bytes([0x12, 25, 0, 0x19, 0x2A])), 25: (0x2A19, b"\x64"),
    }
    return Gatt(a)


def devices(mouse_reachable=True):
    return [
        Device("DC:2C:26:00:00:01", "keyboard", name="Keys-To-Go", cod=0x002540, rssi=-40, pairing="passkey",
               sdp={0x1124: hid_sdp_answer("Keys-To-Go", KB_DESC, 0x40, 0x21),
                    0x1200: pnp_sdp_answer(0x05ac, 0x0255, 0x0050)}),
        Device("AC:37:43:00:00:04", "phone", name="Phone", cod=0x5a020c, rssi=-50, pairing="fail"),
        Device("C4:00:00:00:00:05", "mouse", addr_type=2, name="LE Mouse", appearance=0x03C2, rssi=-55,
               pairing="le", gatt=le_gatt(),
               # maker data (company 0x01DA + 2 bytes) and TX power -8 dBm, for "survey"
               extra=bytes([5, 0xFF, 0xDA, 0x01, 0x01, 0x02, 2, 0x0A, 0xF8])),
        Device("00:0A:95:00:00:03", "keyboard", name="Old Keyboard", cod=0x002540, rssi=-65, pairing="legacy",
               sdp={0x1124: hid_sdp_answer("Old Keyboard", KB_DESC, 0x40)}),
        Device("00:1F:20:00:00:02", "mouse", name="BT Mouse", cod=0x002580, rssi=-70, pairing="justworks",
               eir_name=False, reachable=mouse_reachable,
               sdp={0x1124: hid_sdp_answer("BT Mouse", MOUSE_DESC, 0x80)}),
        Device("11:22:33:00:00:06", "keyboard", name="Dual KB", cod=0x002540, le_also=True, rssi=-80,
               pairing="passkey", sdp={0x1124: hid_sdp_answer("Dual KB", KB_DESC, 0x40)}),
        Device("22:33:44:00:00:07", "keyboard", name="Display KB", cod=0x002540, rssi=-85,
               pairing="displayed", sdp={0x1124: hid_sdp_answer("Display KB", KB_DESC, 0x40)}),
    ]


KB, PHONE, LEM, OLDKB, MOUSE, DUAL, DISP = [d.addr for d in devices()]

TMP = tempfile.mkdtemp(prefix="nzbt-flows-")
FAKE = os.path.join(TMP, "fake")
STORE = os.path.join(TMP, "store")
os.makedirs(FAKE)
ENV = dict(os.environ, NZBT_FAKE_DIR=FAKE, NZEUS_BT_DIR=STORE,
           ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
print("nzeus-bt flow tests: %s %s" % (" ".join(PREFIX), PROG))

# ================= 1. nothing loaded / no controller =================
print("-- Bluetooth not loaded, no controller")
rc, out = run("status")
check(rc == 0 and "loaded=no" in out, "status without the kernel modules says loaded=no")
rc, out = run("on")
check(rc == 1 and "not loaded" in out, "on without the kernel modules fails with a clear message")
fk = FakeKernel(FAKE, devices(), controller=False)
rc, out = run("status")
check("controller=none" in out, "status with no controller says controller=none")
rc, out = run("on")
check(rc == 1 and "no Bluetooth controller" in out, "on with no controller: clear error")
fk.stop()

# ================= 2. on / scan =================
print("-- on, scan")
fk = FakeKernel(FAKE, devices())
rc, out = run("scan", "2")
check(rc == 1 and "Bluetooth is off" in out, "scan while off: tells to run 'nzeus-bt on'")
rc, out = run("on")
check(rc == 0 and out.startswith("on hci0 00:E0:4C:00:00:01, 0 paired devices"), "on: switched on, address shown")
ops = fk.ops()
check(ops.index("SET_POWERED") > ops.index("SET_SSP") and ops.index("SET_POWERED") > ops.index("LOAD_LINK_KEYS"),
      "on: SSP set and keys loaded BEFORE power on")
check(fk.io_cap == 1, "on: I/O capability DisplayYesNo")
check(fk.local_name == b"NZEUS R36S", "on: local name NZEUS R36S")
check(fk.settings & 0x41 == 0x41 and fk.settings & 0x10 and fk.settings & 0x200, "on: powered + SSP + bondable + LE")
check(not fk.settings & 0x02, "on: not connectable for strangers (only paired devices may connect)")
rc, out = run("on")
check(rc == 0 and fk.ops().count("SET_POWERED") == 1, "on twice: no second power-on (idempotent)")

t0 = time.time()
rc, out = run("scan", "3")
took = time.time() - t0
lines = [l.split("\t") for l in out.strip().splitlines()]
check(rc == 0 and len(lines) == 7, "scan: 7 devices, one line each (got %d)" % len(lines))
check(all(len(l) == 6 for l in lines), "scan: 6 tab-separated fields per line")
by = {l[0]: l for l in lines}
check(by.get(KB) == [KB, "Keys-To-Go", "keyboard", "classic", "-40", "yes"], "scan: classic keyboard line")
check(by.get(PHONE) == [PHONE, "Phone", "phone", "classic", "-50", "no"], "scan: phone = not supported")
check(by.get(LEM) == [LEM, "LE Mouse", "mouse", "le", "-55", "yes"], "scan: LE mouse (appearance + HID service)")
check(by.get(MOUSE, [None, None])[1] == "BT Mouse", "scan: name fetched for a device that did not send it")
check(fk.name_confirm.get(MOUSE) == 0 and KB not in fk.name_confirm, "scan: kernel asked to fetch only the missing name")
check(by.get(DUAL, [0, 0, 0, 0])[3] == "dual", "scan: same address on classic + LE = dual")
check([l[0] for l in lines][:3] == [KB, PHONE, LEM], "scan: strongest signal first")
check(fk.ops().count("START_DISCOVERY") >= 2, "scan: restarts the kernel's scan rounds until the time is up")
# (generous upper limit: the WSL virtual machine sometimes pauses for a few
#  seconds - seen in tests; this only has to prove the scan ends by itself)
check(2.9 < took < 12 + 3 * SLOW, "scan: stops by itself after the time (%.1f s)" % took)
check(not fk.discovering, "scan: discovery stopped at the end")
rc, out = run("scan", "0")
check(rc == 2, "scan 0: refused")

# survey: the same scan, with everything each device told (for the deck's ID cards)
rc, out = run("survey", "3")
lines = [l.split("\t") for l in out.strip().splitlines()]
check(rc == 0 and len(lines) == 7 and all(len(l) == 9 for l in lines), "survey: 7 devices, 9 tab-separated fields each")
by = {l[0]: l for l in lines}
check(by.get(LEM) == [LEM, "le-random", "-55", "-8", "-", "03c2", "1812", "01da:0102", "LE Mouse"],
      "survey: LE mouse - random address, TX power, appearance, HID service, maker data (got %s)" % by.get(LEM))
check(by.get(KB) == [KB, "classic", "-40", "-", "002540", "-", "-", "-", "Keys-To-Go"],
      "survey: classic keyboard - its class, nothing else sent (got %s)" % by.get(KB))
check(by.get(DUAL, [0, 0])[1] == "dual", "survey: dual device")
rc, out = run("survey", "200")
check(rc == 2, "survey 200: refused (1..120 s)")

# ================= 3. pairing =================
print("-- pair")
rc, out = run("pair", KB)
check(rc == 0, "pair keyboard: success")
check(re.search(r"^passkey 123456 - type it on the keyboard", out, re.M) is not None, "pair keyboard: passkey shown")
check("typed 3" in out and "typed 6" in out, "pair keyboard: typing progress shown")
check(re.search(r"^paired$", out, re.M) and re.search(r"^connected$", out, re.M), "pair keyboard: paired + connected")
f = store_fields(KB)
check(f is not None, "pair keyboard: saved to the SD card store")
if f:
    check(f.get("linkkey") == fk.link_keys[KB][1].hex(), "pair keyboard: the kernel's key was saved exactly")
    check(f.get("linkkey_type") == "5" and f.get("type") == "classic", "pair keyboard: key type + address type")
    check(f.get("hid_descriptor") == KB_DESC.hex(), "pair keyboard: report descriptor from SDP (in pieces) saved")
    check(f.get("hid_vendor") == "0x05ac" and f.get("hid_product") == "0x0255", "pair keyboard: maker IDs (PnP) saved")
    check(f.get("name") == "Keys-To-Go" and f.get("kind") == "keyboard", "pair keyboard: name + kind saved")
h = fk.hidp.get(KB, {})
check(h.get("rd") == KB_DESC.hex() and h.get("vendor") == "0x05ac" and h.get("name") == "Keys-To-Go",
      "pair keyboard: kernel HID device created with descriptor, vendor, name")
check(h.get("flags") == str(1 << 9) and h.get("country") == "33" and h.get("subclass") == "0x40",
      "pair keyboard: HIDP flags/country/subclass")
check(fk.allowed.get(KB) == (0, 1), "pair keyboard: allowed to reconnect by itself (ADD_DEVICE 1)")
check(oct(os.stat(store_file(KB)).st_mode & 0o777) == "0o600", "pair keyboard: key file readable only by its owner")
check([c[1:] for c in fk.l2cap_connects if c[0] == KB] == [(1, 0, 0), (17, 0, 2), (19, 0, 2)],
      "pair keyboard: SDP, then HID control + interrupt channels with encryption")

rc, out = run("pair", MOUSE)
check(rc == 0 and "confirm - accepting" in out and MOUSE in fk.confirm_replies, "pair mouse: just-works confirmed")
check(MOUSE in fk.hidp and store_fields(MOUSE).get("kind") == "mouse", "pair mouse: connected, kind mouse")

rc, out = run("pair", OLDKB)
m = re.search(r"^pin (\d+) - type it on the keyboard", out, re.M)
check(rc == 0 and m is not None, "pair old keyboard: PIN shown")
if m:
    check(fk.pin_replies[-1] == (OLDKB, m.group(1)) and len(m.group(1)) == 6, "pair old keyboard: same 6-digit PIN sent")
    check(store_fields(OLDKB).get("pin_len") == "6", "pair old keyboard: PIN length saved")

rc, out = run("pair", DISP, stdin="654321\n")
check(rc == 0 and "enter-passkey" in out and fk.passkey_replies[-1] == (DISP, 654321),
      "pair device with its own display: number typed on stdin is sent")

rc, out = run("pair", PHONE)
check(rc == 1 and "error pairing failed: authentication failed" in out, "pair failure: clear error, exit 1")
check(store_fields(PHONE) is None, "pair failure: nothing saved")
rc, out = run("pair", "AA:BB:CC:DD:EE:FF")
check(rc == 1 and "connect failed" in out, "pair a device that is not there: connect failed")
rc, out = run("pair", "AA:BB:CC:DD:EE")
check(rc == 2 and "not a Bluetooth address" in out, "pair bad address: usage error")
rc, out = run("pair", KB, "--pin", "12ab")
check(rc == 2, "pair --pin with letters: refused")

# ================= 4. list / status / disconnect / connect =================
print("-- list, status, disconnect, connect")
rc, out = run("list")
rows = {l.split("\t")[0]: l.split("\t") for l in out.strip().splitlines()}
check(set(rows) == {KB, MOUSE, OLDKB, DISP}, "list: the 4 paired devices")
check(rows.get(KB) == [KB, "Keys-To-Go", "keyboard", "classic", "connected"], "list: keyboard line")
rc, out = run("status")
check("powered=yes" in out and "paired=4" in out and "service=stopped" in out, "status: powered, 4 paired, no service")
rc, out = run("disconnect", KB)
check(rc == 0 and KB not in fk.hidp and KB not in fk.connected, "disconnect: HID closed + link dropped")
rc, out = run("list")
check("%s\tKeys-To-Go\tkeyboard\tclassic\tnot-connected" % KB in out, "list: shows not-connected")
rc, out = run("connect", KB)
check(rc == 0 and out.strip() == "connected" and KB in fk.hidp, "connect: HID device back")
rc, out = run("connect", KB)
check(rc == 0 and "already" in out, "connect again: already connected")
rc, out = run("connect", PHONE)
check(rc == 1 and "not paired" in out, "connect an unpaired device: tells to pair first")

saved_keys = {a: store_fields(a)["linkkey"] for a in (KB, MOUSE, OLDKB, DISP)}
fk.stop()

# ================= 5. restart: keys come back, the service reconnects =================
print("-- after a restart: on + daemon")
fk = FakeKernel(FAKE, devices(mouse_reachable=False))
rc, out = run("on")
check(rc == 0 and "4 paired devices" in out, "on after restart: 4 paired devices")
check({a: k[1].hex() for a, k in fk.link_keys.items()} == saved_keys, "on after restart: all saved keys loaded into the kernel")
check(all(fk.allowed.get(a) == (0, 1) for a in saved_keys), "on after restart: all allowed to reconnect")
daemon = subprocess.Popen(PREFIX + [PROG, "daemon"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, env=ENV)
CHILDREN.append(daemon)
check(wait_for(lambda: os.path.exists(os.path.join(FAKE, "run-daemon.pid")), 5), "daemon: started, pid file written")
check(wait_for(lambda: KB in fk.hidp and OLDKB in fk.hidp and DISP in fk.hidp, 8),
      "daemon: calls every paired keyboard once at start")
rc, out = run("status")
check("service=running" in out, "status: service=running")
rc, out = run("daemon")
check("already running" in out, "second daemon: refuses to start twice")
fk.devices[MOUSE].reachable = True
check(wait_for(lambda: os.path.exists(os.path.join(FAKE, "listen-17")), 3), "daemon: listening for keyboards")
fk.device_calls(MOUSE)
check(wait_for(lambda: MOUSE in fk.hidp, 5), "daemon: a mouse that wakes up and calls us is attached")
check(fk.hidp.get(MOUSE, {}).get("rd") == MOUSE_DESC.hex(), "daemon: with the saved report descriptor")
fk.device_calls(PHONE)
time.sleep(0.5 * SLOW)
check(PHONE not in fk.hidp, "daemon: an unpaired device that calls is refused")
fk.ctrl_waiting.clear()
fk.device_calls(OLDKB, psms=(19,))
time.sleep(0.3 * SLOW)
check(fk.hidp.get(OLDKB) is not None, "daemon: interrupt channel without control channel is ignored")
# events of ANOTHER controller (index 1) reach every mgmt socket: they must be ignored
# (found by the UML test with real 4.4 kernel code and three controllers)
OTHER = "66:55:44:33:22:11"
with fk.lock:
    for c in list(fk.clients):
        fk.send_ev(c, EV_NEW_LINK_KEY, 1, bytes([1]) + addr_bytes(OTHER) + bytes([0, 5]) + b"\x11" * 16 + b"\0")
time.sleep(0.5 * SLOW)
check(store_fields(OTHER) is None, "daemon: a key of ANOTHER controller is not saved")
# a new key arrives while only the daemon is running (device re-keyed the link)
fk.new_link_key(KB, 7)
check(wait_for(lambda: (store_fields(KB) or {}).get("linkkey") == fk.link_keys[KB][1].hex(), 5),
      "daemon: saves a new key it receives")
check(store_fields(KB).get("hid_descriptor") == KB_DESC.hex(), "daemon: the key update keeps the descriptor")

# ================= 6. LE mouse (HID over GATT + uhid) =================
print("-- LE mouse")
rc, out = run("pair", LEM)
check(rc == 0 and "paired" in out and "connecting - the Bluetooth service" in out, "pair LE mouse: paired, service asked")
f = store_fields(LEM) or {}
check(f.get("type") == "le-random" and f.get("kind") == "mouse", "pair LE mouse: LE random address + kind saved")
check(len([l for l in open(store_file(LEM)).read().splitlines() if l.startswith("ltk=")]) == 2 and "irk" in f,
      "pair LE mouse: both LE keys + identity key saved")
check(fk.allowed.get(LEM) == (2, 2), "pair LE mouse: kernel auto-connect on (ADD_DEVICE 2)")
check(wait_for(lambda: fk.uhid and fk.uhid[0]["created"] is not None, 8), "LE mouse: virtual HID device created")
if fk.uhid and fk.uhid[0]["created"]:
    c = fk.uhid[0]["created"]
    check(c["rd"] == REPORT_MAP, "LE mouse: report map read completely (long read)")
    check(c["name"] == "LE Mouse" and c["uniq"] == LEM and c["bus"] == 5, "LE mouse: name, address, bus=Bluetooth")
    check(c["vendor"] == 0x046d and c["product"] == 0x1234 and c["version"] == 1, "LE mouse: maker IDs from PnP ID")
    check(c["country"] == 0x21, "LE mouse: country from HID information")
    check(not fk.uhid[0].get("bad_sizes"), "LE mouse: uhid messages have the kernel's size")
g = fk.devices[LEM].gatt
check(wait_for(lambda: (14, b"\x01\x00", True) in g.writes and (21, b"\x01\x00", True) in g.writes, 5),
      "LE mouse: notifications switched on for both input reports")
check((18, b"\x01\x00", True) not in g.writes, "LE mouse: not for the output report")
fk.att_send(LEM, bytes([0x1B, 13, 0]) + bytes([0x01, 0x05, 0xfb, 0x00]))
fk.att_send(LEM, bytes([0x1B, 20, 0]) + bytes([0, 0, 0x04, 0, 0, 0, 0, 0]))
check(wait_for(lambda: len(fk.uhid[0]["inputs"]) >= 2, 5), "LE mouse: messages forwarded")
if len(fk.uhid[0]["inputs"]) >= 2:
    check(fk.uhid[0]["inputs"][0] == bytes([1, 0x01, 0x05, 0xfb, 0x00]), "LE mouse: mouse report gets its ID 1 in front")
    check(fk.uhid[0]["inputs"][1] == bytes([2, 0, 0, 0x04, 0, 0, 0, 0, 0]), "LE mouse: keyboard report gets ID 2")
fk.uhid_kernel_sends(fk.uhid[0], UHID_OUTPUT, bytes([2, 0x01]).ljust(4096, b"\0") + bytes([2, 0, 1]))
check(wait_for(lambda: (17, b"\x01", False) in g.writes, 5), "LE mouse: Caps Lock light sent to the output report")
fk.uhid_kernel_sends(fk.uhid[0], UHID_GET_REPORT, bytes([7, 0, 0, 0, 1, 3]))
check(wait_for(lambda: fk.uhid[0].get("get_report_replies"), 5) and
      fk.uhid[0]["get_report_replies"][0][:6] == bytes([7, 0, 0, 0, 5, 0]),
      "LE mouse: report request answered at once with an error")
fk.att_send(LEM, bytes([0x02, 100, 0]))                       # the device asks OUR MTU
fk.att_send(LEM, bytes([0x10, 1, 0, 0xff, 0xff, 0, 0x28]))    # ...and our services
check(wait_for(lambda: getattr(g, "client_mtu_answer", 0) == 185, 5), "LE mouse: device's MTU question answered")
check(wait_for(lambda: any(e[1] == 0x10 and e[4] == 0x0A for e in getattr(g, "client_errors", [])), 5),
      "LE mouse: device's service question answered 'not found'")
with fk.lock:
    fk.disconnect(LEM)
check(wait_for(lambda: fk.uhid[0]["destroyed"], 5), "LE mouse: virtual device removed when it disconnects")
fk.le_connect(LEM)
check(wait_for(lambda: len(fk.uhid) >= 2 and fk.uhid[1]["created"] is not None, 8),
      "LE mouse: attached again when the kernel reconnects it")

# ================= 7. remove / off / stop =================
print("-- remove, off, stop")
rc, out = run("remove", KB)
check(rc == 0 and store_fields(KB) is None and KB not in fk.link_keys and KB not in fk.hidp, "remove: key, file, connection gone")
check(addr_bytes(KB) + b"\0" in fk.ops("REMOVE_DEVICE"), "remove: no longer allowed to reconnect")
rc, out = run("remove", KB)
check(rc == 0 and "was not paired" in out, "remove twice: harmless")
rc, out = run("off")
check(rc == 0 and out.strip() == "off" and not fk.settings & 1, "off: powered off")
rc, out = run("status")
check("powered=no" in out and "paired=4" in out, "status after off: powered=no, 4 still paired")
daemon.send_signal(signal.SIGTERM)
try:
    log, _ = daemon.communicate(timeout=10 * SLOW)
except subprocess.TimeoutExpired:
    daemon.kill()
    log, _ = daemon.communicate()
check(daemon.returncode == 0 and "service stopped" in log, "daemon: stops cleanly on SIGTERM")
check(not os.path.exists(os.path.join(FAKE, "run-daemon.pid")), "daemon: pid file removed")
# the dongle is unplugged while the service runs: it must stop by itself
daemon2 = subprocess.Popen(PREFIX + [PROG, "daemon"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, env=ENV)
CHILDREN.append(daemon2)
wait_for(lambda: os.path.exists(os.path.join(FAKE, "run-daemon.pid")), 5)
time.sleep(0.5 * SLOW)
fk.emit(0x0005, b"")                                   # MGMT_EV_INDEX_REMOVED for hci0
try:
    log2, _ = daemon2.communicate(timeout=5 * SLOW)
except subprocess.TimeoutExpired:
    daemon2.kill()
    log2, _ = daemon2.communicate()
check("controller removed" in log2 and daemon2.returncode == 0, "daemon: stops when the dongle is unplugged")
check("refused (not paired)" in log and "reconnected" in log, "daemon log: refused + reconnected lines")
check("ERROR: AddressSanitizer" not in log and "runtime error" not in log, "daemon: no memory errors")
print("     daemon log:\n       " + log.strip().replace("\n", "\n       "))
check(not fk.unbound_connects, "every data channel was bound to our controller")
check(not [e for e in fk.log if e[0] == "EXCEPTION"], "fake kernel: no internal errors")
fk.stop()

shutil.rmtree(TMP, ignore_errors=True)
print("flow tests: %d checks, %d failed" % (CHECKS[0], len(FAILS)))
sys.exit(1 if FAILS else 0)
