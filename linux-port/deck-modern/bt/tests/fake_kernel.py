#!/usr/bin/env python3
"""fake_kernel.py - pretends to be the console's Bluetooth kernel modules
(and the Bluetooth devices around it) so nzeus-bt can be tested on a PC.

The test build of nzeus-bt (-DNZBT_FAKE) connects to Unix sockets in a folder
instead of real Bluetooth sockets:
    mgmt         the management socket (same binary messages as the kernel)
    l2cap        data channels: first message "CONNECT addr type psm cid sec"
    hidp         HIDP ioctls as text ("DEL addr", "LIST")
    uhid         /dev/uhid (same binary uhid_event messages as the kernel)
    listen-<psm> created BY nzeus-bt; our fake devices connect there
The message layouts follow the console's kernel (Linux 4.4.186): mgmt.h,
hidp.h, uhid.h. Everything that happens is recorded for the tests.
"""
import os
import selectors
import socket
import struct
import threading
import time
import heapq

# ---- mgmt numbers (include/net/bluetooth/mgmt.h, Linux 4.4) ----
OP = dict(READ_INDEX_LIST=0x03, READ_INFO=0x04, SET_POWERED=0x05, SET_CONNECTABLE=0x07,
          SET_BONDABLE=0x09, SET_SSP=0x0B, SET_LE=0x0D, SET_DEV_CLASS=0x0E,
          SET_LOCAL_NAME=0x0F, LOAD_LINK_KEYS=0x12, LOAD_LONG_TERM_KEYS=0x13,
          DISCONNECT=0x14, GET_CONNECTIONS=0x15, PIN_CODE_REPLY=0x16, PIN_CODE_NEG_REPLY=0x17,
          SET_IO_CAPABILITY=0x18, PAIR_DEVICE=0x19, UNPAIR_DEVICE=0x1B,
          USER_CONFIRM_REPLY=0x1C, USER_CONFIRM_NEG_REPLY=0x1D, USER_PASSKEY_REPLY=0x1E,
          USER_PASSKEY_NEG_REPLY=0x1F, START_DISCOVERY=0x23, STOP_DISCOVERY=0x24,
          CONFIRM_NAME=0x25, SET_SECURE_CONN=0x2D, LOAD_IRKS=0x30, ADD_DEVICE=0x33,
          REMOVE_DEVICE=0x34)
OPNAME = {v: k for k, v in OP.items()}
EV_CMD_COMPLETE, EV_CMD_STATUS = 0x01, 0x02
EV_NEW_SETTINGS, EV_NEW_LINK_KEY, EV_NEW_LTK = 0x06, 0x09, 0x0A
EV_DEVICE_CONNECTED, EV_DEVICE_DISCONNECTED = 0x0B, 0x0C
EV_PIN_CODE_REQUEST, EV_USER_CONFIRM_REQUEST, EV_USER_PASSKEY_REQUEST = 0x0E, 0x0F, 0x10
EV_AUTH_FAILED, EV_DEVICE_FOUND, EV_DISCOVERING = 0x11, 0x12, 0x13
EV_DEVICE_UNPAIRED, EV_PASSKEY_NOTIFY, EV_NEW_IRK = 0x16, 0x17, 0x18
ST_OK, ST_NOT_CONNECTED, ST_CONNECT_FAILED, ST_AUTH_FAILED = 0x00, 0x02, 0x04, 0x05
ST_REJECTED, ST_INVALID, ST_NOT_POWERED, ST_UNKNOWN = 0x0B, 0x0D, 0x0F, 0x01
S_POWERED, S_CONNECTABLE, S_BONDABLE, S_SSP, S_BREDR, S_LE, S_SC = 1, 2, 0x10, 0x40, 0x80, 0x200, 0x800

# uhid event numbers (include/uapi/linux/uhid.h)
UHID_DESTROY, UHID_OUTPUT, UHID_CREATE2, UHID_INPUT2 = 1, 6, 11, 12
UHID_GET_REPORT, UHID_GET_REPORT_REPLY = 9, 10
CONTROLLER_ADDR = "00:E0:4C:00:00:01"      # the fake console's own Bluetooth address
UHID_EVENT_SIZE = 4 + 128 + 64 + 64 + 2 + 2 + 16 + 4096   # the biggest member: create2


def addr_bytes(s):
    """'AA:BB:CC:DD:EE:FF' -> 6 bytes, backwards like the kernel"""
    return bytes(int(x, 16) for x in reversed(s.split(":")))


def addr_text(b):
    return ":".join("%02X" % x for x in reversed(b[:6]))


# ---- SDP data elements (big-endian), built independently of nzeus-bt's parser ----
def de_uint8(v): return bytes([0x08, v])
def de_uint16(v): return bytes([0x09]) + struct.pack(">H", v)
def de_uint32(v): return bytes([0x0A]) + struct.pack(">I", v)
def de_uuid16(v): return bytes([0x19]) + struct.pack(">H", v)
def de_text(b):
    b = b.encode() if isinstance(b, str) else b
    return (bytes([0x25, len(b)]) if len(b) < 256 else bytes([0x26]) + struct.pack(">H", len(b))) + b
def de_seq(*items):
    body = b"".join(items)
    return (bytes([0x35, len(body)]) if len(body) < 256 else bytes([0x36]) + struct.pack(">H", len(body))) + body
def de_record(attrs):
    return de_seq(*[de_uint16(k) + v for k, v in sorted(attrs.items())])


def hid_sdp_answer(name, desc, subclass, country=0, parser=0x0111):
    rec = de_record({0x0000: de_uint32(0x10001), 0x0001: de_seq(de_uuid16(0x1124)),
                     0x0100: de_text(name), 0x0201: de_uint16(parser), 0x0202: de_uint8(subclass),
                     0x0203: de_uint8(country), 0x0206: de_seq(de_seq(de_uint8(0x22), de_text(desc)))})
    return de_seq(rec)


def pnp_sdp_answer(vendor, product, version, source=1):
    rec = de_record({0x0000: de_uint32(0x10002), 0x0001: de_seq(de_uuid16(0x1200)),
                     0x0201: de_uint16(vendor), 0x0202: de_uint16(product),
                     0x0203: de_uint16(version), 0x0205: de_uint16(source)})
    return de_seq(rec)


class Device:
    """A Bluetooth device near the fake console."""
    def __init__(self, addr, kind, addr_type=0, name="", cod=0, appearance=None, rssi=-60,
                 pairing="passkey", eir_name=True, sdp=None, gatt=None, le_also=False,
                 reachable=True, extra=b""):
        self.addr, self.kind, self.addr_type, self.name = addr, kind, addr_type, name
        self.cod, self.appearance, self.rssi, self.pairing = cod, appearance, rssi, pairing
        self.eir_name, self.sdp, self.gatt, self.le_also = eir_name, sdp or {}, gatt, le_also
        self.reachable = reachable
        self.extra = extra           # more advert records, sent as they are (maker data, TX power...)

    def eir(self, with_name):
        e = b""
        if self.addr_type != 0:
            e += bytes([2, 0x01, 0x06])
            if self.appearance is not None:
                e += bytes([3, 0x19]) + struct.pack("<H", self.appearance)
            if self.kind in ("mouse", "keyboard"):
                e += bytes([3, 0x03, 0x12, 0x18])
        if with_name and self.name:
            n = self.name.encode()
            e += bytes([len(n) + 1, 0x09]) + n
        if self.addr_type == 0 and self.cod:
            e += bytes([4, 0x0D]) + struct.pack("<I", self.cod)[:3]
        return e + self.extra


class Gatt:
    """A tiny GATT database: list of (handle, type uuid16, value bytes)."""
    def __init__(self, attrs):
        self.attrs = attrs           # handle -> (uuid16, value)
        self.writes = []             # (handle, value, with_response)

    def handles(self):
        return sorted(self.attrs)


class FakeKernel:
    def __init__(self, folder, devices=(), controller=True, discovery_round=1.0):
        self.dir = folder
        self.devices = {d.addr: d for d in devices}
        self.controller = controller
        self.round = discovery_round
        self.log = []                 # (op name, params) of every mgmt command received
        self.settings = S_BREDR       # powered off, classic on
        self.supported = S_POWERED | S_CONNECTABLE | S_BONDABLE | S_SSP | S_BREDR | S_LE | S_SC
        self.io_cap = None
        self.local_name = b""
        self.link_keys = {}           # addr -> (type, key16, pin_len)   (loaded or new)
        self.ltks = {}                # addr -> list of 36-byte entries
        self.irks = {}
        self.allowed = {}             # ADD_DEVICE: addr -> (type, action)
        self.connected = {}           # addr -> addr type
        self.hidp = {}                # addr -> dict of the HIDPCONNADD request
        self.uhid = []                # list of dicts: create2 info + inputs
        self.discovering = 0
        self.name_confirm = {}        # addr -> name_known
        self.resolved = set()         # names fetched once stay known (the kernel's inquiry cache)
        self.pin_replies = []
        self.confirm_replies = []
        self.passkey_replies = []
        self.l2cap_connects = []      # (addr, psm, cid, sec)
        self.unbound_connects = []    # calls not bound to our controller (a bug)
        self.att_sockets = {}         # addr -> socket (LE ATT channel to nzeus-bt)
        self.att_from_client = []     # raw ATT messages nzeus-bt sent to devices
        self.pending_pair = None
        self.clients = []             # mgmt sockets
        self.sel = selectors.DefaultSelector()
        self.timers = []
        self.tseq = 0
        self.stop_flag = False
        self.lock = threading.RLock()
        self.counter = 0
        self.dev_sockets = []         # our "device side" sockets to nzeus-bt listeners
        self.ctrl_waiting = {}        # addr -> (ctrl socket, got intr?)
        os.makedirs(folder, exist_ok=True)
        self.servers = {}
        for name in ("mgmt", "l2cap", "hidp", "uhid"):
            path = os.path.join(folder, name)
            if os.path.exists(path):
                os.unlink(path)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            s.bind(path)
            s.listen(16)
            s.setblocking(False)
            self.sel.register(s, selectors.EVENT_READ, ("accept", name))
            self.servers[name] = s
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    # ---------------- plumbing ----------------
    def later(self, delay, fn, *args):
        with self.lock:
            self.tseq += 1
            heapq.heappush(self.timers, (time.time() + delay, self.tseq, fn, args))

    def run(self):
        while not self.stop_flag:
            with self.lock:
                wait = 0.05
                if self.timers:
                    wait = max(0.0, min(wait, self.timers[0][0] - time.time()))
            for key, _ in self.sel.select(wait):
                with self.lock:
                    try:
                        self.on_ready(key)
                    except Exception as e:     # a test bug must not kill the fake silently
                        import traceback
                        traceback.print_exc()
                        self.log.append(("EXCEPTION", repr(e)))
            with self.lock:
                while self.timers and self.timers[0][0] <= time.time():
                    _, _, fn, args = heapq.heappop(self.timers)
                    try:
                        fn(*args)
                    except Exception as e:
                        import traceback
                        traceback.print_exc()
                        self.log.append(("EXCEPTION", repr(e)))

    def stop(self):
        self.stop_flag = True
        self.thread.join(2)
        for s in list(self.servers.values()):
            s.close()

    def on_ready(self, key):
        kind, info = key.data
        sock = key.fileobj
        if kind == "accept":
            try:
                c, _ = sock.accept()
            except BlockingIOError:
                return
            c.setblocking(False)
            if info == "mgmt":
                self.clients.append(c)
                self.sel.register(c, selectors.EVENT_READ, ("mgmt", None))
            elif info == "l2cap":
                self.sel.register(c, selectors.EVENT_READ, ("l2cap-new", None))
            elif info == "hidp":
                self.sel.register(c, selectors.EVENT_READ, ("hidp", None))
            elif info == "uhid":
                dev = {"created": None, "inputs": [], "destroyed": False, "sock": c}
                self.uhid.append(dev)
                self.sel.register(c, selectors.EVENT_READ, ("uhid", dev))
            return
        try:
            data = sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            data = b""
        if not data:
            self.drop(sock, kind, info)
            return
        getattr(self, "on_" + kind.replace("-", "_"))(sock, data, info)

    def drop(self, sock, kind, info):
        try:
            self.sel.unregister(sock)
        except Exception:
            pass
        if sock in self.clients:
            self.clients.remove(sock)
        if kind == "att":
            self.att_sockets.pop(info, None)
        if kind == "uhid" and info is not None:
            info["closed"] = True
        sock.close()

    # ---------------- mgmt ----------------
    def send_ev(self, sock, ev, index, params):
        try:
            sock.send(struct.pack("<HHH", ev, index, len(params)) + params)
        except OSError:
            pass

    def broadcast(self, ev, params, skip=None):
        for c in list(self.clients):
            if c is not skip:
                self.send_ev(c, ev, 0, params)

    def complete(self, sock, op, status, params=b""):
        self.send_ev(sock, EV_CMD_COMPLETE, 0 if op != OP["READ_INDEX_LIST"] else 0xFFFF,
                     struct.pack("<HB", op, status) + params)

    def settings_bytes(self):
        return struct.pack("<I", self.settings)

    def on_mgmt(self, sock, data, info):
        op, index, plen = struct.unpack_from("<HHH", data)
        p = data[6:]
        self.log.append((OPNAME.get(op, hex(op)), p))
        if len(p) != plen:
            return self.complete(sock, op, ST_INVALID)
        if op == OP["READ_INDEX_LIST"]:
            return self.complete(sock, op, 0, struct.pack("<HH", 1, 0) if self.controller else b"\0\0")
        if not self.controller or index != 0:
            return self.send_ev(sock, EV_CMD_STATUS, index, struct.pack("<HB", op, 0x11))
        handler = getattr(self, "op_" + OPNAME.get(op, "unknown").lower(), None)
        if handler is None:
            return self.complete(sock, op, ST_UNKNOWN)
        handler(sock, op, p)

    def op_read_info(self, sock, op, p):
        info = addr_bytes(CONTROLLER_ADDR) + bytes([6]) + struct.pack("<H", 0x5D)
        info += struct.pack("<II", self.supported, self.settings) + b"\x10\x01\x00"
        info += self.local_name[:249].ljust(249, b"\0") + b"\0" * 11
        self.complete(sock, op, 0, info)

    def set_bit(self, sock, op, p, bit):
        if len(p) != 1 or p[0] > 1:
            return self.complete(sock, op, ST_INVALID)
        self.settings = (self.settings | bit) if p[0] else (self.settings & ~bit)
        self.complete(sock, op, 0, self.settings_bytes())
        self.broadcast(EV_NEW_SETTINGS, self.settings_bytes(), skip=sock)

    def op_set_powered(self, sock, op, p):
        if p == b"\0":
            self.connected.clear()
            self.hidp.clear()
        self.set_bit(sock, op, p, S_POWERED)
    def op_set_ssp(self, sock, op, p): self.set_bit(sock, op, p, S_SSP)
    def op_set_bondable(self, sock, op, p): self.set_bit(sock, op, p, S_BONDABLE)
    def op_set_le(self, sock, op, p): self.set_bit(sock, op, p, S_LE)
    def op_set_secure_conn(self, sock, op, p): self.set_bit(sock, op, p, S_SC)
    def op_set_connectable(self, sock, op, p): self.set_bit(sock, op, p, S_CONNECTABLE)

    def op_set_io_capability(self, sock, op, p):
        if len(p) != 1 or p[0] > 4:
            return self.complete(sock, op, ST_INVALID)
        self.io_cap = p[0]
        self.complete(sock, op, 0)

    def op_set_local_name(self, sock, op, p):
        if len(p) != 260:
            return self.complete(sock, op, ST_INVALID)
        self.local_name = p[:249].split(b"\0")[0]
        self.complete(sock, op, 0, p)

    def op_set_dev_class(self, sock, op, p):
        if len(p) != 2 or p[1] & 3:
            return self.complete(sock, op, ST_INVALID)
        self.complete(sock, op, 0, b"\x10\x01\x00")

    def op_load_link_keys(self, sock, op, p):
        if len(p) < 3:
            return self.complete(sock, op, ST_INVALID)
        n = struct.unpack_from("<H", p, 1)[0]
        if len(p) != 3 + 25 * n:                 # the kernel checks this exactly
            return self.complete(sock, op, ST_INVALID)
        self.link_keys = {}
        for i in range(n):
            k = p[3 + 25 * i: 3 + 25 * (i + 1)]
            if k[6] != 0:                         # must be a classic address
                return self.complete(sock, op, ST_INVALID)
            self.link_keys[addr_text(k)] = (k[7], k[8:24], k[24])
        self.complete(sock, op, 0)

    def op_load_long_term_keys(self, sock, op, p):
        if len(p) < 2:
            return self.complete(sock, op, ST_INVALID)
        n = struct.unpack_from("<H", p)[0]
        if len(p) != 2 + 36 * n:
            return self.complete(sock, op, ST_INVALID)
        self.ltks = {}
        for i in range(n):
            k = p[2 + 36 * i: 2 + 36 * (i + 1)]
            if k[6] not in (1, 2) or k[8] > 1:    # LE address + master 0/1 (ltk_is_valid)
                return self.complete(sock, op, ST_INVALID)
            self.ltks.setdefault(addr_text(k), []).append(k)
        self.complete(sock, op, 0)

    def op_load_irks(self, sock, op, p):
        if len(p) < 2:
            return self.complete(sock, op, ST_INVALID)
        n = struct.unpack_from("<H", p)[0]
        if len(p) != 2 + 23 * n:
            return self.complete(sock, op, ST_INVALID)
        self.irks = {addr_text(p[2 + 23 * i:]): p[2 + 23 * i + 7: 2 + 23 * i + 23] for i in range(n)}
        self.complete(sock, op, 0)

    def op_add_device(self, sock, op, p):
        if len(p) != 8:
            return self.complete(sock, op, ST_INVALID)
        a, t, action = addr_text(p), p[6], p[7]
        if action > 2 or (t == 0 and action != 1):    # 4.4: classic only "allow incoming"
            return self.complete(sock, op, ST_INVALID, p[:7])
        self.allowed[a] = (t, action)
        self.complete(sock, op, 0, p[:7])

    def op_remove_device(self, sock, op, p):
        if len(p) != 7:
            return self.complete(sock, op, ST_INVALID)
        self.allowed.pop(addr_text(p), None)
        self.complete(sock, op, 0, p[:7])

    def op_get_connections(self, sock, op, p):
        if not self.settings & S_POWERED:
            return self.complete(sock, op, ST_NOT_POWERED)
        body = b"".join(addr_bytes(a) + bytes([t]) for a, t in self.connected.items())
        self.complete(sock, op, 0, struct.pack("<H", len(self.connected)) + body)

    def op_disconnect(self, sock, op, p):
        a = addr_text(p)
        if a not in self.connected:
            return self.complete(sock, op, ST_NOT_CONNECTED, p[:7])
        self.disconnect(a)
        self.complete(sock, op, 0, p[:7])

    def disconnect(self, a, reason=2):
        t = self.connected.pop(a, None)
        self.hidp.pop(a, None)
        s = self.att_sockets.pop(a, None)
        if s is not None:
            self.drop(s, "att", a)
        if t is not None:
            self.broadcast(EV_DEVICE_DISCONNECTED, addr_bytes(a) + bytes([t, reason]))

    # ---- discovery ----
    def op_start_discovery(self, sock, op, p):
        if not self.settings & S_POWERED:
            return self.complete(sock, op, ST_NOT_POWERED, p)
        if self.discovering:
            return self.complete(sock, op, 0x0A, p)       # busy
        self.discovering = p[0]
        self.counter += 1
        rnd = self.counter
        self.complete(sock, op, 0, p)
        self.broadcast(EV_DISCOVERING, bytes([p[0], 1]))
        delay = 0.1
        for d in self.devices.values():
            types = [d.addr_type] + ([1] if d.le_also else [])
            for t in types:
                if (t == 0 and not p[0] & 1) or (t != 0 and not p[0] & 6):
                    continue
                self.later(delay, self.found, d, t, rnd)
                delay += 0.05
        self.later(self.round, self.end_round, rnd)

    def found(self, d, t, rnd):
        if not self.discovering or rnd != self.counter:
            return
        with_name = d.eir_name or t != 0 or d.addr in self.resolved
        flags = 1 if (t == 0 and not with_name) else 0          # CONFIRM_NAME
        eir = d.eir(with_name) if t == d.addr_type else bytes([2, 1, 6])
        self.broadcast(EV_DEVICE_FOUND, addr_bytes(d.addr) + bytes([t, d.rssi & 0xff]) +
                       struct.pack("<IH", flags, len(eir)) + eir)

    def op_confirm_name(self, sock, op, p):
        a = addr_text(p)
        self.name_confirm[a] = p[7]
        self.complete(sock, op, 0, p[:7])
        d = self.devices.get(a)
        if d and p[7] == 0 and d.name:                          # "remote name request"
            self.resolved.add(a)
            eir = bytes([len(d.name) + 1, 0x09]) + d.name.encode()
            self.later(0.1, lambda: self.discovering and self.broadcast(
                EV_DEVICE_FOUND, addr_bytes(a) + bytes([0, d.rssi & 0xff]) +
                struct.pack("<IH", 0, len(eir)) + eir))

    def end_round(self, rnd):
        if rnd != self.counter or not self.discovering:
            return
        t = self.discovering
        self.discovering = 0
        self.broadcast(EV_DISCOVERING, bytes([t, 0]))

    def op_stop_discovery(self, sock, op, p):
        if not self.discovering or p[0] != self.discovering:
            return self.complete(sock, op, ST_REJECTED, p)
        self.discovering = 0
        self.complete(sock, op, 0, p)
        self.broadcast(EV_DISCOVERING, bytes([p[0], 0]))

    # ---- pairing ----
    def new_link_key(self, a, ktype=5, pin_len=0):
        key = os.urandom(16)
        self.link_keys[a] = (ktype, key, pin_len)
        self.broadcast(EV_NEW_LINK_KEY, bytes([1]) + addr_bytes(a) + bytes([0, ktype]) + key + bytes([pin_len]))

    def op_pair_device(self, sock, op, p):
        if len(p) != 8:
            return self.complete(sock, op, ST_INVALID)
        a, t = addr_text(p), p[6]
        if not self.settings & S_POWERED:
            return self.complete(sock, op, ST_NOT_POWERED, p[:7])
        d = self.devices.get(a)
        if d is None or not d.reachable:
            return self.later(0.3, self.complete, sock, op, ST_CONNECT_FAILED, p[:7])
        self.connected[a] = t
        self.broadcast(EV_DEVICE_CONNECTED, p[:7] + struct.pack("<IH", 0, 0))
        self.pending_pair = dict(sock=sock, addr=a, type=t, dev=d, op=op)
        if d.pairing == "passkey":            # keyboard: we show, it types
            ab = addr_bytes(a) + bytes([t])
            self.broadcast(EV_PASSKEY_NOTIFY, ab + struct.pack("<IB", 123456, 0))
            self.later(0.1, self.broadcast, EV_PASSKEY_NOTIFY, ab + struct.pack("<IB", 123456, 3))
            self.later(0.2, self.broadcast, EV_PASSKEY_NOTIFY, ab + struct.pack("<IB", 123456, 6))
            self.later(0.3, self.finish_pair, True)
        elif d.pairing == "justworks":        # mouse: kernel asks us to confirm
            self.broadcast(EV_USER_CONFIRM_REQUEST, addr_bytes(a) + bytes([t, 1]) + struct.pack("<I", 0))
            self.later(3.0, self.finish_pair, False)
        elif d.pairing in ("legacy", "legacy-mouse"):
            self.broadcast(EV_PIN_CODE_REQUEST, addr_bytes(a) + bytes([t, 0]))
            self.later(3.0, self.finish_pair, False)
        elif d.pairing == "le":
            self.later(0.2, self.finish_pair, True)
        elif d.pairing == "displayed":        # the device shows a number, we type it
            self.broadcast(EV_USER_PASSKEY_REQUEST, addr_bytes(a) + bytes([t]))
            self.later(3.0, self.finish_pair, False)
        else:                                 # "fail"
            self.later(0.2, self.finish_pair, False)

    def finish_pair(self, ok, pin_len=0):
        pp = self.pending_pair
        if pp is None:
            return
        self.pending_pair = None
        a, t = pp["addr"], pp["type"]
        ab = addr_bytes(a) + bytes([t])
        if ok:
            if t == 0:
                self.new_link_key(a, 0 if pp["dev"].pairing.startswith("legacy") else 5, pin_len)
            else:
                for master in (1, 0):
                    k = ab + bytes([1, master, 16]) + struct.pack("<HQ", 0x1234 + master, 0x1122334455667788 + master) + os.urandom(16)
                    self.ltks.setdefault(a, []).append(k)
                    self.broadcast(EV_NEW_LTK, bytes([1]) + k)
                irk = os.urandom(16)
                self.irks[a] = irk
                self.broadcast(EV_NEW_IRK, bytes([1]) + b"\0" * 6 + ab + irk)
            self.complete(pp["sock"], pp["op"], ST_OK, ab)
        else:
            self.broadcast(EV_AUTH_FAILED, ab + bytes([ST_AUTH_FAILED]))
            self.connected.pop(a, None)
            self.complete(pp["sock"], pp["op"], ST_AUTH_FAILED, ab)

    def op_user_confirm_reply(self, sock, op, p):
        self.confirm_replies.append(addr_text(p))
        self.complete(sock, op, 0, p[:7])
        if self.pending_pair and self.pending_pair["addr"] == addr_text(p):
            self.later(0.05, self.finish_pair, True)

    def op_pin_code_reply(self, sock, op, p):
        if len(p) != 24:
            return self.complete(sock, op, ST_INVALID)
        pin = p[8:8 + p[7]].decode()
        self.pin_replies.append((addr_text(p), pin))
        self.complete(sock, op, 0, p[:7])
        pp = self.pending_pair
        if pp and pp["addr"] == addr_text(p):
            ok = pin == "0000" if pp["dev"].pairing == "legacy-mouse" else len(pin) >= 4
            self.later(0.05, self.finish_pair, ok, len(pin))

    def op_user_passkey_reply(self, sock, op, p):
        self.passkey_replies.append((addr_text(p), struct.unpack_from("<I", p, 7)[0]))
        self.complete(sock, op, 0, p[:7])
        if self.pending_pair:
            self.later(0.05, self.finish_pair, struct.unpack_from("<I", p, 7)[0] == 654321)

    def op_user_passkey_neg_reply(self, sock, op, p):
        self.complete(sock, op, 0, p[:7])
        self.later(0.05, self.finish_pair, False)

    def op_unpair_device(self, sock, op, p):
        a = addr_text(p)
        known = a in self.link_keys or a in self.ltks
        self.link_keys.pop(a, None)
        self.ltks.pop(a, None)
        self.irks.pop(a, None)
        if p[7]:
            self.disconnect(a)
        if not known:
            return self.complete(sock, op, 0x06, p[:7])       # not paired
        self.complete(sock, op, 0, p[:7])
        self.broadcast(EV_DEVICE_UNPAIRED, p[:7], skip=sock)

    # ---------------- L2CAP channels ----------------
    def on_l2cap_new(self, sock, data, info):
        parts = data.decode().split()
        if len(parts) != 7 or parts[0] != "CONNECT":
            sock.send(b"ERR 22")
            return
        a, t, psm, cid, sec = parts[1], int(parts[2]), int(parts[3]), int(parts[4]), int(parts[5])
        self.l2cap_connects.append((a, psm, cid, sec))
        if parts[6] != CONTROLLER_ADDR:          # must be bound to OUR controller
            self.unbound_connects.append(parts[6])
            return sock.send(b"ERR 101")       # ENETUNREACH
        d = self.devices.get(a)
        if not self.settings & S_POWERED:
            return sock.send(b"ERR 100")                       # ENETDOWN
        if d is None or not d.reachable:
            return sock.send(b"ERR 112")                       # EHOSTDOWN
        paired = a in self.link_keys or a in self.ltks
        if sec >= 2 and not paired:
            return sock.send(b"ERR 13")                        # EACCES: needs pairing
        self.sel.unregister(sock)
        if psm == 1:
            self.sel.register(sock, selectors.EVENT_READ, ("sdp", d))
        elif psm in (17, 19):
            if t != 0:
                return sock.send(b"ERR 22")
            self.connected.setdefault(a, 0)
            self.sel.register(sock, selectors.EVENT_READ, ("hidchan", (d, psm)))
        elif cid == 4 and t != 0 and d.gatt is not None:
            self.connected.setdefault(a, t)
            self.att_sockets[a] = sock
            self.sel.register(sock, selectors.EVENT_READ, ("att", a))
        else:
            return sock.send(b"ERR 111")
        sock.send(b"OK")

    def on_sdp(self, sock, data, d):
        # Service Search Attribute request -> answer in pieces of 48 bytes
        if data[0] != 0x06 or len(data) < 5:
            return
        tid = data[1:3]
        uuid = struct.unpack_from(">H", data, 8)[0]
        offset = 0
        # byte 19 = length of the continuation token; ours is a 2-byte offset
        clen = data[19] if len(data) > 19 else 0
        if clen:
            offset = struct.unpack(">H", data[20:22])[0]
        answer = d.sdp.get(uuid, de_seq())
        piece = answer[offset:offset + 48]
        rest = offset + len(piece) < len(answer)
        cont = bytes([2]) + struct.pack(">H", offset + len(piece)) if rest else b"\0"
        body = struct.pack(">H", len(piece)) + piece + cont
        sock.send(bytes([0x07]) + tid + struct.pack(">H", len(body)) + body)

    def on_hidchan(self, sock, data, info):
        d, psm = info
        if psm == 19 and data == b"HIDP-INTR":
            self.ctrl_waiting.setdefault(d.addr, {})["intr"] = True
            return
        if psm == 17 and data.startswith(b"HIDPCONNADD"):
            return self.hidp_add(sock, d.addr, data)

    def hidp_add(self, sock, a, data, tries=0):
        if not self.ctrl_waiting.get(a, {}).get("intr"):
            # the interrupt channel's message may simply not be read yet
            if tries < 20:
                return self.later(0.05, self.hidp_add, sock, a, data, tries + 1)
            return sock.send(b"ERR 77")                         # EBADFD: no interrupt channel
        self.ctrl_waiting.pop(a, None)
        if a in self.hidp:
            return sock.send(b"ERR 17")                         # EEXIST
        fields = {}
        text = data.decode()
        head, _, name = text.partition(" name=")
        for kv in head.split()[1:]:
            k, _, v = kv.partition("=")
            fields[k] = v
        fields["name"] = name
        self.hidp[a] = fields
        self.connected[a] = 0
        sock.send(b"OK")

    def on_hidp(self, sock, data, info):
        parts = data.decode().split()
        if parts[0] == "LIST":
            sock.send(("OK " + ",".join(self.hidp)).encode())
        elif parts[0] == "DEL":
            if parts[1] in self.hidp:
                del self.hidp[parts[1]]
                sock.send(b"OK")
            else:
                sock.send(b"ERR 2")
        else:
            sock.send(b"ERR 22")

    # devices calling nzeus-bt (it listens like the real daemon)
    def device_calls(self, addr, psms=(17, 19), send_attach_answer=True):
        socks = []
        for psm in psms:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            s.connect(os.path.join(self.dir, "listen-%d" % psm))
            s.send(("FROM " + addr).encode())
            s.setblocking(False)
            self.sel.register(s, selectors.EVENT_READ, ("incoming", (addr, psm)))
            socks.append(s)
            self.dev_sockets.append(s)
            time.sleep(0.05)
        return socks

    def on_incoming(self, sock, data, info):
        addr, psm = info
        if psm == 19 and data == b"HIDP-INTR":
            self.ctrl_waiting.setdefault(addr, {})["intr"] = True
        elif psm == 17 and data.startswith(b"HIDPCONNADD"):
            self.hidp_add(sock, addr, data)

    # ---------------- ATT (LE) ----------------
    def on_att(self, sock, data, a):
        self.att_from_client.append(data)
        d = self.devices[a]
        g = d.gatt
        op = data[0]
        mtu = getattr(g, "mtu", 23)

        def err(code, handle=0):
            sock.send(bytes([0x01, op]) + struct.pack("<H", handle) + bytes([code]))

        if op == 0x02:                                         # MTU
            g.mtu = min(struct.unpack_from("<H", data, 1)[0], 64)
            sock.send(bytes([0x03]) + struct.pack("<H", 64))
        elif op == 0x03:                                       # answer to OUR mtu request
            g.client_mtu_answer = struct.unpack_from("<H", data, 1)[0]
        elif op == 0x01:
            g.client_errors = getattr(g, "client_errors", []) + [data]
        elif op == 0x10:                                       # read by group type
            s, e, t = struct.unpack_from("<HHH", data, 1)
            out = b""
            for h in g.handles():
                u, v = g.attrs[h]
                if s <= h <= e and u == t:
                    end = self.group_end(g, h)
                    item = struct.pack("<HH", h, end) + v
                    if out and len(item) != out_len:
                        break
                    if len(out) + len(item) + 2 > mtu:
                        break
                    out_len = len(item)
                    out += item
            if not out:
                return err(0x0A, s)
            sock.send(bytes([0x11, out_len]) + out)
        elif op == 0x08:                                       # read by type
            s, e, t = struct.unpack_from("<HHH", data, 1)
            out = b""
            for h in g.handles():
                u, v = g.attrs[h]
                if s <= h <= e and u == t:
                    item = struct.pack("<H", h) + v
                    if out and len(item) != out_len:
                        break
                    if len(out) + len(item) + 2 > mtu:
                        break
                    out_len = len(item)
                    out += item
            if not out:
                return err(0x0A, s)
            sock.send(bytes([0x09, out_len]) + out)
        elif op == 0x04:                                       # find information
            s, e = struct.unpack_from("<HH", data, 1)
            out = b""
            for h in g.handles():
                if s <= h <= e:
                    if len(out) + 4 + 2 > mtu:
                        break
                    out += struct.pack("<HH", h, g.attrs[h][0])
            if not out:
                return err(0x0A, s)
            sock.send(bytes([0x05, 1]) + out)
        elif op in (0x0A, 0x0C):                               # read / read blob
            h = struct.unpack_from("<H", data, 1)[0]
            off = struct.unpack_from("<H", data, 3)[0] if op == 0x0C else 0
            if h not in g.attrs:
                return err(0x01, h)
            v = g.attrs[h][1]
            if off > len(v):
                return err(0x07, h)
            sock.send(bytes([op + 1]) + v[off:off + mtu - 1])
        elif op == 0x12:                                       # write request
            h = struct.unpack_from("<H", data, 1)[0]
            g.writes.append((h, data[3:], True))
            sock.send(b"\x13")
        elif op == 0x52:                                       # write command
            h = struct.unpack_from("<H", data, 1)[0]
            g.writes.append((h, data[3:], False))

    def group_end(self, g, h):
        hs = g.handles()
        nxt = [x for x in hs if x > h and g.attrs[x][0] in (0x2800, 0x2801)]
        return (nxt[0] - 1) if nxt else 0xFFFF

    def att_send(self, addr, data):
        s = self.att_sockets.get(addr)
        if s:
            s.send(data)
            return True
        return False

    # ---------------- uhid ----------------
    def on_uhid(self, sock, data, dev):
        t = struct.unpack_from("<I", data)[0]
        if len(data) != UHID_EVENT_SIZE:
            dev.setdefault("bad_sizes", []).append(len(data))
        if t == UHID_CREATE2:
            name = data[4:132].split(b"\0")[0].decode()
            uniq = data[196:260].split(b"\0")[0].decode()
            rd_size, bus, vendor, product, version, country = struct.unpack_from("<HHIIII", data, 260)
            rd = data[280:280 + rd_size]
            dev["created"] = dict(name=name, uniq=uniq, bus=bus, vendor=vendor, product=product,
                                  version=version, country=country, rd=rd)
        elif t == UHID_INPUT2:
            size = struct.unpack_from("<H", data, 4)[0]
            dev["inputs"].append(data[6:6 + size])
        elif t == UHID_DESTROY:
            dev["destroyed"] = True
        elif t == UHID_GET_REPORT_REPLY:
            dev.setdefault("get_report_replies", []).append(data[4:12])

    def uhid_kernel_sends(self, dev, etype, payload):
        """the kernel's HID side talks to nzeus-bt (LED output, report requests)"""
        ev = struct.pack("<I", etype) + payload
        dev["sock"].send(ev.ljust(UHID_EVENT_SIZE, b"\0"))

    # ---------------- helpers for tests ----------------
    def ops(self, name=None):
        with self.lock:
            return [p for (n, p) in self.log if name is None or n == name] if name else [n for n, _ in self.log]

    def emit(self, ev, params):
        with self.lock:
            self.broadcast(ev, params)

    def le_connect(self, addr):
        """the kernel's auto-connect found the LE device"""
        with self.lock:
            d = self.devices[addr]
            self.connected[addr] = d.addr_type
            self.broadcast(EV_DEVICE_CONNECTED, addr_bytes(addr) + bytes([d.addr_type]) + struct.pack("<IH", 0, 0))
