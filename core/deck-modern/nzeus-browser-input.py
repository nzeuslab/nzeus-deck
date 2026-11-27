#!/usr/bin/python3
"""nzeus-browser-input.py FIFO - the console's buttons for NetSurf (the Deck's Web app).

NetSurf (Debian's netsurf-fb) draws through SDL 1.2's "fbcon" driver. SDL reads
the mouse from a device speaking the old PS/2 mouse protocol, and the keyboard
from the text console. This kernel has no /dev/input/mice, so this bridge:
  - GRABS the R36S button devices (nothing else sees their keys meanwhile, so
    the D-pad no longer scrolls the page by itself),
  - writes PS/2 mouse packets into FIFO (SDL_MOUSEDEV=FIFO, SDL_MOUSEDRV=PS2),
  - types chosen keys through a virtual keyboard (/dev/uinput), which reach
    NetSurf through the text console like a real keyboard.

  D-pad / stick  move the pointer (held = it speeds up)
  A              click (held = drag)
  B              back (clicks NetSurf's own Back arrow: it has no Back key)
  L1 / R1        scroll a little up / down        L2 / R2   a page up / down
  X              zoom: bigger, bigger, back to normal
  Y              type an address or a search (exit 10: the Deck asks, then reopens)
  SELECT+START   quit (exit 0)

The pointer position is tracked here (SDL moves it 1:1 and stops at the edges);
it is put in a known place first by pushing it into the top-left corner.
Plain Python, no extra modules. Started by nzeus-browser.
"""
import os
import select
import struct
import sys
import time

W, H = 640, 480
BACK_X, BACK_Y = 12, 14                    # NetSurf's Back arrow (toolbar, top left)
EVENT = struct.Struct('llHHi')             # struct input_event on 32-bit: time, type, code, value
EV_SYN, EV_KEY = 0, 1
# the R36S buttons send KEYBOARD codes (nzeus-modern.c keymap)
K_UP, K_DOWN, K_LEFT, K_RIGHT = 103, 108, 105, 106
K_A, K_B, K_X, K_Y = 29, 56, 57, 42                 # Left Ctrl, Left Alt, Space, Left Shift
K_L1, K_R1, K_L2, K_R2 = 15, 14, 104, 109           # Tab, Backspace, Page Up, Page Down
K_SELECT, K_START = 1, 28                            # Esc, Enter
# keys typed into NetSurf
OUT_UP, OUT_DOWN, OUT_PGUP, OUT_PGDN = 103, 108, 104, 109
OUT_CTRL, OUT_EQUAL, OUT_0 = 29, 13, 11              # NetSurf zooms with Ctrl + '=' / Ctrl + '0'


def ioc(direction, kind, nr, size):
    """Linux _IOC: MIPS puts the direction bits differently from x86/ARM"""
    if os.uname().machine.startswith('mips'):
        bits = {'none': 1, 'read': 2, 'write': 4}[direction]
        return bits << 29 | size << 16 | ord(kind) << 8 | nr
    bits = {'none': 0, 'write': 1, 'read': 2}[direction]
    return bits << 30 | size << 16 | ord(kind) << 8 | nr


EVIOCGRAB = ioc('write', 'E', 0x90, 4)
UI_SET_EVBIT = ioc('write', 'U', 100, 4)
UI_SET_KEYBIT = ioc('write', 'U', 101, 4)
UI_DEV_CREATE = ioc('none', 'U', 1, 0)
UI_DEV_DESTROY = ioc('none', 'U', 2, 0)


def ioctl_int(fd, request, value):
    import fcntl
    fcntl.ioctl(fd, request, value)


def button_devices():
    """the console's own button devices, grabbed (nothing else gets their keys now)"""
    found = []
    for name in sorted(os.listdir('/dev/input')):
        if not name.startswith('event'):
            continue
        try:
            with open('/sys/class/input/%s/device/name' % name) as f:
                dev = f.read().strip()
        except OSError:
            continue
        if dev in ('r36s-keys', 'serializer-keys'):
            fd = os.open('/dev/input/' + name, os.O_RDONLY | os.O_NONBLOCK)
            try:
                ioctl_int(fd, EVIOCGRAB, 1)
            except OSError:
                pass                                    # (still works, keys may leak to the page)
            found.append(fd)
    return found


class Keyboard:
    """a virtual keyboard: keys typed into NetSurf through the text console"""

    def __init__(self):
        self.fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
        ioctl_int(self.fd, UI_SET_EVBIT, EV_KEY)
        for k in (OUT_UP, OUT_DOWN, OUT_PGUP, OUT_PGDN, OUT_CTRL, OUT_EQUAL, OUT_0):
            ioctl_int(self.fd, UI_SET_KEYBIT, k)
        # struct uinput_user_dev: name[80], input_id (4 x u16), ff_effects_max, 4 x 64 ints
        dev = b'nzeus-browser-keys'.ljust(80, b'\0') + struct.pack('HHHHi', 6, 0x1d6b, 0x0104, 1, 0)
        os.write(self.fd, dev + b'\0' * (4 * 64 * 4))
        ioctl_int(self.fd, UI_DEV_CREATE, 0)
        time.sleep(0.3)                                  # (the console keyboard attaches to it)

    def key(self, key, value):
        os.write(self.fd, EVENT.pack(0, 0, EV_KEY, key, value) + EVENT.pack(0, 0, EV_SYN, 0, 0))

    def tap(self, key, ctrl=False):
        if ctrl:
            self.key(OUT_CTRL, 1)
        self.key(key, 1)
        self.key(key, 0)
        if ctrl:
            self.key(OUT_CTRL, 0)

    def close(self):
        try:
            ioctl_int(self.fd, UI_DEV_DESTROY, 0)
        except OSError:
            pass
        os.close(self.fd)


class Mouse:
    """PS/2 packets into the FIFO; remembers where the pointer is"""

    def __init__(self, fifo):
        self.fd = os.open(fifo, os.O_WRONLY)              # (waits until SDL opens it)
        self.x, self.y, self.buttons = 0, 0, 0

    def send(self, dx, dy):                              # dy > 0 = down on screen
        while dx or dy:
            sx, sy = max(-200, min(200, dx)), max(-200, min(200, dy))
            head = 0x08 | (self.buttons & 3) | (0x10 if sx < 0 else 0) | (0x20 if -sy < 0 else 0)
            os.write(self.fd, bytes([head, sx & 0xff, (-sy) & 0xff]))
            dx, dy = dx - sx, dy - sy

    def move(self, dx, dy):
        nx, ny = max(0, min(W - 1, self.x + dx)), max(0, min(H - 1, self.y + dy))
        self.send(nx - self.x, ny - self.y)
        self.x, self.y = nx, ny

    def home(self):                                      # into the top-left corner: a known place
        self.send(-2 * W, -2 * H)
        self.x, self.y = 0, 0

    def press(self, buttons):
        self.buttons = buttons
        os.write(self.fd, bytes([0x08 | (buttons & 3), 0, 0]))

    def click_at(self, x, y):
        """click (x, y), then put the pointer back where it was"""
        bx, by = self.x, self.y
        self.home()
        self.move(x, y)
        time.sleep(0.05)
        self.press(1)
        time.sleep(0.08)
        self.press(0)
        time.sleep(0.05)
        self.home()
        self.move(bx, by)


def main():
    mouse = Mouse(sys.argv[1])
    keys = Keyboard()
    devices = button_devices()
    mouse.home()
    mouse.move(W // 2, H // 2)
    held = set()
    zoom = 0
    speed, last_move = 2, 0.0
    try:
        while True:
            ready, _, _ = select.select(devices, [], [], 0.02)
            for fd in ready:
                data = os.read(fd, EVENT.size * 32)
                for i in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    _, _, kind, code, value = EVENT.unpack_from(data, i)
                    if kind != EV_KEY or value == 2:
                        continue
                    if value:
                        held.add(code)
                    else:
                        held.discard(code)
                    if K_SELECT in held and K_START in held:
                        return 0                         # quit
                    if not value:
                        if code == K_A:
                            mouse.press(0)
                        continue
                    if code == K_A:
                        mouse.press(1)
                    elif code == K_B:
                        mouse.click_at(BACK_X, BACK_Y)   # NetSurf's Back arrow
                    elif code == K_Y:
                        return 10                        # the Deck asks for an address
                    elif code == K_X:
                        zoom = (zoom + 1) % 3            # bigger, bigger, normal (each step +20 %)
                        for _ in range(2 if zoom else 1):
                            keys.tap(OUT_EQUAL if zoom else OUT_0, ctrl=True)
                    elif code in (K_L1, K_R1, K_L2, K_R2):
                        keys.tap({K_L1: OUT_UP, K_R1: OUT_DOWN, K_L2: OUT_PGUP, K_R2: OUT_PGDN}[code])
            dx = (K_RIGHT in held) - (K_LEFT in held)
            dy = (K_DOWN in held) - (K_UP in held)
            now = time.monotonic()
            if dx or dy:
                if now - last_move > 0.03:
                    speed = min(speed + 1, 14) if last_move and now - last_move < 0.1 else 2
                    mouse.move(dx * speed, dy * speed)
                    last_move = now
            else:
                last_move = 0.0
    finally:
        keys.close()


if __name__ == '__main__':
    sys.exit(main())
