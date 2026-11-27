#!/usr/bin/env python3
"""PC test of nz-sdboot (the MIPS binary) under qemu-mipsel with fake trees.

Every case builds a fake /proc, /sys and /dev in a temporary folder (with the
NZ-SDBOOT-FAKE-TREE marker) holding a REAL header made by
ports/r36s-hichip/rootfs/region-tool/nz_region.py and a real ext4 superblock from
mke2fs, then runs `nz-sdboot --fake-root DIR` and checks the exit code and the
reason it prints. Also tests --check-init on fake SD roots (static, dynamic,
usrmerge symlinks, scripts, wrong ABI). Proves the program logic only: no
real card, kernel, sysfs or input device is involved.

  python3 test-nz-sdboot.py --binary path/to/nz-sdboot [--busybox path/to/mips/busybox]
Needs: qemu-mipsel, mke2fs, mipsel-linux-gnu-gcc (for the dynamic test init).
"""
import argparse
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'rootfs' / 'region-tool'))
import nz_region  # noqa: E402  (read-only use)

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--binary', required=True)
ap.add_argument('--busybox', help='static MIPS BusyBox used as a static /sbin/init')
ap.add_argument('--qemu', default=shutil.which('qemu-mipsel') or 'qemu-mipsel')
ap.add_argument('--report', help='write a JSON report here')
args = ap.parse_args()
BIN = str(Path(args.binary).resolve())
CID = nz_region.TEST_CARD_CID
LAYOUT = nz_region.compute_layout(nz_region.CARD_BYTES, nz_region.SNAPSHOT_BYTES)
UUIDS = {n: nz_region.fs_uuid(n, CID) for n in ('nzroot', 'nzdata')}
HEADER = nz_region.make_header(LAYOUT, CID, '00' * 32, UUIDS)
work = Path(tempfile.mkdtemp(prefix='nz-sdboot-test-'))
results = []

# One real ext4 superblock area (first 64 KiB of a small mke2fs image).
ext4 = work / 'root.ext4'
subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', '-L', 'nzroot', '-U', UUIDS['nzroot'], str(ext4), '8M'],
               check=True, stdout=subprocess.DEVNULL)
EXT4_HEAD = ext4.read_bytes()[:65536]


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


def fake_tree(name):
    t = work / name
    write(t / 'NZ-SDBOOT-FAKE-TREE', 'test tree for nz-sdboot\n')
    write(t / 'proc/cmdline', 'console=ttyS0,19200 earlycon rdinit=/init ro panic=0 ' + LAYOUT['blkdevparts'] + '\n')
    write(t / 'sys/block/mmcblk0/device/cid', CID + '\n')
    write(t / 'sys/block/mmcblk0/size', '%d\n' % (nz_region.CARD_BYTES // 512))
    for p in LAYOUT['parts']:
        d = t / ('sys/class/block/mmcblk0p%d' % p['num'])
        write(d / 'start', '%d\n' % (p['start'] // 512))
        write(d / 'size', '%d\n' % (p['bytes'] // 512))
        write(d / 'ro', '1\n' if p['ro'] else '0\n')
        write(d / 'dev', '179:%d\n' % p['num'])
        write(d / 'uevent', 'MAJOR=179\nMINOR=%d\nDEVNAME=mmcblk0p%d\nDEVTYPE=partition\nDISKSEQ=1\nPARTN=%d\n'
                            'PARTNAME=%s\n' % (p['num'], p['num'], p['num'], p['name']))
        content = {2: HEADER + bytes(4096 - 512), 5: HEADER + bytes(4096 - 512), 3: EXT4_HEAD}.get(p['num'], bytes(4096))
        write(t / ('dev/mmcblk0p%d' % p['num']), content)
    write(t / 'sys/class/input/event0/device/name', 'r36s-keys\n')
    write(t / 'sys/class/input/event1/device/name', 'serializer-keys\n')
    write(t / 'dev/input/event0', '0')
    write(t / 'dev/input/event1', '0')
    return t


def run(argv, timeout=60):
    """Run nz-sdboot; output to a file (a stuck child must not hold a pipe)."""
    out = work / 'out.txt'
    with open(out, 'wb') as f:
        proc = subprocess.Popen([args.qemu, BIN] + argv, stdout=f, stderr=subprocess.STDOUT,
                                start_new_session=True)
        try:
            code = proc.wait(timeout=timeout)
        finally:
            try:
                os.killpg(proc.pid, signal.SIGKILL)   # also a child left stuck on purpose
            except ProcessLookupError:
                pass
    return code, out.read_text(errors='replace')


def case(name, tree, expect_code, expect_text, extra=(), timeout=60):
    start = time.monotonic()
    code, text = run(['--fake-root', str(tree), *extra], timeout)
    took = time.monotonic() - start
    ok = code == expect_code and expect_text in text
    last = [line for line in text.splitlines() if line.strip()]
    results.append(dict(case=name, ok=ok, exit=code, expected_exit=expect_code, seconds=round(took, 1),
                        expected_text=expect_text, last_lines=last[-2:]))
    print('%s %-46s exit=%s (%.1fs)' % ('PASS' if ok else 'FAIL', name, code, took))
    if not ok:
        print(text)


def header_with(**changes):
    """A valid (correct CRC) header with changed fields."""
    layout = json.loads(json.dumps(LAYOUT))
    cid, fsha = CID, '00' * 32
    if 'part0_ro' in changes:
        layout['parts'][0]['ro'] = changes['part0_ro']
    if 'fsha' in changes:
        fsha = changes['fsha']
    if 'card_bytes' in changes:
        layout['card_bytes'] = changes['card_bytes']
    return nz_region.make_header(layout, cid, fsha, UUIDS)


def put(t, num, data):
    write(t / ('dev/mmcblk0p%d' % num), data + bytes(4096 - len(data)))


# ------------------------------------------------------------ main decision
case('pass: everything matches', fake_tree('pass'), 0, 'NZ_SDBOOT_OK')
t = fake_tree('log')
case('pass: --log file written', t, 0, 'NZ_SDBOOT_OK', ['--log', str(t / 'run.log')])
results[-1]['ok'] &= (t / 'run.log').read_text().strip().endswith('NZ_SDBOOT_OK')
t = fake_tree('ramonly')
write(t / 'proc/cmdline', 'console=ttyS0,19200 nzeus.ramonly ' + LAYOUT['blkdevparts'] + '\n')
case('escape: nzeus.ramonly on the command line', t, 4, 'nzeus.ramonly')
t = fake_tree('fn')
write(t / 'dev/input/event0', '1')
case('escape: FN held', t, 4, 'FN is held')
t = fake_tree('nokeys')
shutil.rmtree(t / 'sys/class/input/event0')
case('refuse: no r36s-keys input device', t, 1, 'no input device named r36s-keys')
t = fake_tree('crc2')
bad = bytearray(HEADER)
bad[200] ^= 1
put(t, 2, bytes(bad))
case('refuse: bad CRC in p2 header', t, 1, 'mmcblk0p2 (nzhdr): header CRC mismatch')
t = fake_tree('crc5')
put(t, 5, bytes(bad))
case('refuse: bad CRC in p5 (backup) header', t, 1, 'mmcblk0p5 (nzhdr2): header CRC mismatch')
t = fake_tree('magic')
put(t, 2, b'X' + HEADER[1:])
case('refuse: no header magic', t, 1, 'no NZEUS header magic')
t = fake_tree('zeros')
put(t, 2, bytes(512))
case('refuse: empty header block', t, 1, 'no NZEUS header magic')
t = fake_tree('differ')
put(t, 5, header_with(fsha='11' * 32))
case('refuse: both headers valid but different', t, 1, 'both valid but differ')
t = fake_tree('partro')
h = header_with(part0_ro=False)
put(t, 2, h)
put(t, 5, h)
case('refuse: header says nzstock is not read-only', t, 1, 'part 1 is nzstock (kind 0, ro 0)')
t = fake_tree('cid')
write(t / 'sys/block/mmcblk0/device/cid', '000000000000000000000000000000b8\n')
case('refuse: card CID differs from header', t, 1, 'wrong card')
t = fake_tree('cardsize')
write(t / 'sys/block/mmcblk0/size', '%d\n' % (nz_region.CARD_BYTES // 512 - 8))
case('refuse: card size differs from header', t, 1, 'card size')
t = fake_tree('start3')
write(t / 'sys/class/block/mmcblk0p3/start', '%d\n' % (LAYOUT['parts'][2]['start'] // 512 + 8))
case('refuse: p3 start differs (wrong start)', t, 1, 'mmcblk0p3 starts at sector')
t = fake_tree('start1')
write(t / 'sys/class/block/mmcblk0p1/start', '2049\n')
case('refuse: p1 start differs', t, 1, 'mmcblk0p1 starts at sector 2049')
t = fake_tree('size4')
write(t / 'sys/class/block/mmcblk0p4/size', '%d\n' % (LAYOUT['parts'][3]['bytes'] // 512 - 1))
case('refuse: p4 size differs (card shortened it)', t, 1, 'mmcblk0p4 has')
t = fake_tree('name3')
write(t / 'sys/class/block/mmcblk0p3/uevent', 'MAJOR=179\nMINOR=3\nPARTN=3\nPARTNAME=nzdata\n')
case('refuse: p3 has another name', t, 1, 'mmcblk0p3 is not named nzroot')
for num in (1, 2, 5):
    t = fake_tree('ro%d' % num)
    write(t / ('sys/class/block/mmcblk0p%d/ro' % num), '0\n')
    case('refuse: p%d not read-only in the kernel' % num, t, 1, 'mmcblk0p%d (%s) is NOT read-only'
         % (num, LAYOUT['parts'][num - 1]['name']))
t = fake_tree('p6')
write(t / 'sys/class/block/mmcblk0p6/dev', '179:6\n')
case('refuse: an extra partition p6', t, 1, 'mmcblk0p6 exists')
t = fake_tree('noext4')
put(t, 3, bytes(4096))
case('refuse: no ext4 in p3', t, 1, 'holds no ext2/3/4')
t = fake_tree('uuid')
other = bytearray(EXT4_HEAD)
other[1024 + 0x68] ^= 0xff
write(t / 'dev/mmcblk0p3', bytes(other))
case('refuse: p3 ext4 UUID is not the header UUID', t, 1, 'is not the header\'s nzroot UUID')
t = fake_tree('nodev')
shutil.rmtree(t / 'sys/class/block/mmcblk0p2')
(t / 'dev/mmcblk0p2').unlink()
case('refuse: missing devices (no p2), waits then stays', t, 1, 'mmcblk0p2 did not appear within 2 s',
     ['--wait', '2'])
t = fake_tree('nodevnode')
(t / 'dev/mmcblk0p5').unlink()
case('refuse: sysfs has p5 but no /dev node', t, 1, 'mmcblk0p5 did not appear within 2 s', ['--wait', '2'])
t = fake_tree('nocard')
shutil.rmtree(t / 'sys/class/block')
case('refuse: no card at all', t, 1, 'mmcblk0p1 did not appear within 1 s', ['--wait', '1'])
t = fake_tree('hang')
(t / 'sys/block/mmcblk0/device/cid').unlink()
os.mkfifo(t / 'sys/block/mmcblk0/device/cid')      # reading it blocks like a dead card
case('no answer: a read that never returns -> exit 3', t, 3, 'no answer within 3 s', ['--timeout', '3'])

# ------------------------------------------------------------ argument safety
code, text = run(['--fake-root', str(work)])
results.append(dict(case='usage: fake root without marker refused', ok=code == 2 and 'needs a test tree' in text, exit=code))
code, text = run(['--fake-root', '/'])
results.append(dict(case='usage: fake root "/" refused', ok=code == 2, exit=code))
code, text = run(['--wait', '1'])
results.append(dict(case='usage: --wait without --fake-root refused', ok=code == 2 and 'only for --fake-root' in text, exit=code))
code, text = run(['--bogus', 'x'])
results.append(dict(case='usage: unknown option refused', ok=code == 2, exit=code))
for r in results[-4:]:
    print('%s %s exit=%s' % ('PASS' if r['ok'] else 'FAIL', r['case'], r['exit']))

# ------------------------------------------------------------ --check-init
cc = shutil.which('mipsel-linux-gnu-gcc')
dyn = work / 'hello-dyn'
src = work / 'hello.c'
src.write_text('int main(void) { return 0; }\n')
subprocess.run([cc, '-O2', '-o', str(dyn), str(src)], check=True)
loader = Path('/usr/mipsel-linux-gnu/lib/ld.so.1')
static = Path(args.busybox) if args.busybox else Path(BIN)


def newroot(name):
    t = fake_tree('init-' + name)
    nr = t / 'newroot'
    for d in ('dev', 'proc', 'sys', 'run'):
        (nr / d).mkdir(parents=True, exist_ok=True)
    return t, nr


def install(path, src_path, mode=0o755):
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src_path, path)
    path.chmod(mode)


def init_case(name, nr, t, expect_code, expect_text):
    code, text = run(['--fake-root', str(t), '--check-init', str(nr)])
    ok = code == expect_code and expect_text in text
    results.append(dict(case='check-init: ' + name, ok=ok, exit=code, expected_exit=expect_code,
                        expected_text=expect_text))
    print('%s check-init: %-38s exit=%s' % ('PASS' if ok else 'FAIL', name, code))
    if not ok:
        print(text)


t, nr = newroot('static')
install(nr / 'sbin/init', static)
init_case('static MIPS init', nr, t, 0, 'NZ_SDBOOT_INIT_OK')
t, nr = newroot('dyn-noloader')
install(nr / 'sbin/init', dyn)
init_case('dynamic init, loader missing', nr, t, 1, '/lib/ld.so.1 is missing')
t, nr = newroot('dyn')
install(nr / 'sbin/init', dyn)
install(nr / 'lib/ld.so.1', loader)
init_case('dynamic init with its loader', nr, t, 0, 'NZ_SDBOOT_INIT_OK')
t, nr = newroot('usrmerge')      # Debian bookworm: /sbin -> usr/sbin, /lib -> usr/lib
install(nr / 'usr/sbin/init', dyn)
install(nr / 'usr/lib/ld.so.1', loader)
os.symlink('usr/sbin', nr / 'sbin')
os.symlink('usr/lib', nr / 'lib')
init_case('usrmerge relative symlinks', nr, t, 0, 'NZ_SDBOOT_INIT_OK')
t, nr = newroot('abslink')
install(nr / 'usr/sbin/init', static)
(nr / 'sbin').mkdir()
os.symlink('/usr/sbin/init', nr / 'sbin/init')    # absolute: must resolve inside the SD root
init_case('absolute symlink resolved inside root', nr, t, 0, 'NZ_SDBOOT_INIT_OK')
t, nr = newroot('missing')
init_case('no /sbin/init', nr, t, 1, '/sbin/init is missing')
t, nr = newroot('x86')
install(nr / 'sbin/init', '/bin/true')
init_case('PC (x86-64) program', nr, t, 1, 'not a 32-bit little-endian ELF')
t, nr = newroot('nan2008')
data = bytearray(static.read_bytes())
struct.pack_into('<I', data, 36, struct.unpack_from('<I', data, 36)[0] | 0x400)
write(nr / 'sbin/init', bytes(data))
(nr / 'sbin/init').chmod(0o755)
init_case('NaN2008 program', nr, t, 1, 'NaN2008')
t, nr = newroot('noexec')
install(nr / 'sbin/init', static, 0o644)
init_case('init not executable', nr, t, 1, 'not an executable file')
t, nr = newroot('script')
write(nr / 'sbin/init', '#!/bin/sh\nexec /bin/sh\n')
(nr / 'sbin/init').chmod(0o755)
install(nr / 'bin/sh', static)
init_case('script init with /bin/sh', nr, t, 0, 'NZ_SDBOOT_INIT_OK')
t, nr = newroot('script-noint')
write(nr / 'sbin/init', '#!/bin/missing-shell\n')
(nr / 'sbin/init').chmod(0o755)
init_case('script init, interpreter missing', nr, t, 1, '/bin/missing-shell is missing')

passed = sum(1 for r in results if r['ok'])
print('NZ_SDBOOT_TESTS %d/%d passed' % (passed, len(results)))
if args.report:
    Path(args.report).write_text(json.dumps(dict(
        status='PASS_PC_QEMU_ONLY' if passed == len(results) else 'FAIL', passed=passed, total=len(results),
        binary_sha256=__import__('hashlib').sha256(Path(BIN).read_bytes()).hexdigest(),
        header_crc='%08x' % zlib.crc32(HEADER[:508]), cases=results,
        limits=['fake /proc, /sys and /dev under qemu-mipsel; no real card, kernel or input device']),
        indent=2) + '\n')
shutil.rmtree(work, ignore_errors=True)
sys.exit(0 if passed == len(results) else 1)
