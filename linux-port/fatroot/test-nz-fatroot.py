#!/usr/bin/env python3
"""PC tests for nz-fatroot (run in WSL from the repo root):
    python3 linux-port/fatroot/test-nz-fatroot.py

Builds nz-fatroot twice: for the console (static MIPS, run under qemu-mipsel)
and for this PC (x86, used for the real loop-device tests, which need sudo).
Fake cards come from tools/tests/fakecard.py. Never touches a real card.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'tests'))
from fakecard import make_fake_card  # noqa: E402

ROOT_SIZE = 3758096384                # nzroot.img on a real card (3.5 GiB)
failures = []


def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name + ('' if cond or not detail else f'  ({detail})'), flush=True)
    if not cond:
        failures.append(name)


tmp = Path(tempfile.mkdtemp(prefix='nzpub-fatroot-', dir='/tmp'))
src = HERE / 'nz-fatroot.c'
mips, x86 = tmp / 'nz-fatroot-mips', tmp / 'nz-fatroot-x86'
subprocess.run(['mipsel-linux-gnu-gcc', '-Os', '-static', '-s', '-march=mips32r2', '-mabi=32',
                '-Wall', '-Wextra', '-Werror', '-o', str(mips), str(src)], check=True)
subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-o', str(x86), str(src)], check=True)


def run_mips(*args, env=None, timeout=30):
    return subprocess.run(['qemu-mipsel', str(mips), *map(str, args)], capture_output=True,
                          text=True, env={**os.environ, **(env or {})}, timeout=timeout)


# ---- find: where is the FAT32 on the card? ----
card = make_fake_card(tmp / 'card.img')
r = run_mips('find', card)
check('find_scrambled', r.returncode == 0 and r.stdout == 'FAT_OFFSET=1048576\nFAT_SIZE=50331648\n', r.stdout + r.stderr)

plain = make_fake_card(tmp / 'plain.img', scrambled=False)
r = run_mips('find', plain)
check('find_plain_mbr', r.returncode == 0 and 'FAT_OFFSET=1048576' in r.stdout, r.stdout + r.stderr)

real16 = make_fake_card(tmp / 'real16.img', boot_code=True, start_lba=32)
r = run_mips('find', real16)
check('find_card_with_boot_code', r.returncode == 0 and 'FAT_OFFSET=16384' in r.stdout, r.stdout + r.stderr)

two = make_fake_card(tmp / 'two.img', extra_partition=True)
stick = make_fake_card(tmp / 'stick.img', files={'photos/a.jpg': b'x'})
r1, r2 = run_mips('find', two), run_mips('find', stick)
check('find_rejects_usb_stick', r1.returncode == 3 and r2.returncode == 3
      and r1.stdout.startswith('fatroot:') and r2.stdout.startswith('fatroot:'), r1.stdout + r2.stdout)

fifo = tmp / 'dead-card'
os.mkfifo(fifo)                                    # reading it blocks, like a card that never answers
t0 = time.time()
r = run_mips('find', fifo, timeout=30)
# (gives up after 10 s; the limit leaves room for qemu's start-up on a busy PC - it failed once at 12)
check('find_unreadable_dev', r.returncode == 3 and time.time() - t0 <= 20 and 'did not answer' in r.stdout,
      r.stdout + r.stderr)


# ---- attach --dry-run: the exact order of steps, with files in a fake /fat ----
def fake_root(root_size=ROOT_SIZE, root=True, data=True):
    d = Path(tempfile.mkdtemp(prefix='root-', dir=tmp))
    (d / 'fat' / 'nzeus').mkdir(parents=True)
    if root:
        with open(d / 'fat/nzeus/nzroot.img', 'wb') as f:
            f.truncate(root_size)
    if data:
        with open(d / 'fat/nzeus/nzdata.img', 'wb') as f:
            f.truncate(64 * 1024 * 1024)
    return d


d = fake_root()
r = run_mips('attach', card, '/newroot', '--dry-run', env={'NZ_ROOT': str(d)})
expected = ('loop ' + str(card) + ' offset=1048576 size=50331648 -> fat\n'
            'mount vfat /fat noatime,flush\n'
            'loop /fat/nzeus/nzroot.img -> root\n'
            'fsck root\n'
            'loop /fat/nzeus/nzdata.img -> data\n'
            'fsck data\n'
            'mount ext4 /newroot ro\n'
            'mount ext4 /data0 noatime\n')
check('attach_dry_run_order', r.returncode == 0 and r.stdout == expected, r.stdout + r.stderr)

r = run_mips('attach', card, '/newroot', '--dry-run', env={'NZ_ROOT': str(fake_root(root=False))})
check('attach_missing_root_image', r.returncode == 4 and 'nzroot.img' in r.stdout, r.stdout)

r = run_mips('attach', card, '/newroot', '--dry-run', env={'NZ_ROOT': str(fake_root(root_size=1000))})
check('attach_wrong_root_size', r.returncode == 4 and 'size' in r.stdout, r.stdout)

r = run_mips('attach', card, '/newroot', '--dry-run', env={'NZ_ROOT': str(fake_root(data=False))})
check('attach_missing_data_image', r.returncode == 4 and 'nzdata.img' in r.stdout, r.stdout)


# ---- show: a message on the console's screen (a fake 640x480 RGB565 framebuffer file) ----
fb = tmp / 'fb0'
fb.write_bytes(bytes(640 * 480 * 2))
r = run_mips('show', 'NZEUS could not start: nzroot.img is missing on the SD card (run install.bat again)',
             env={'NZ_FB': str(fb), 'NZ_FB_GEOMETRY': '640x480x16'})
px = fb.read_bytes()
corner = px[0:2]
WHITE = bytes([255, 255])
white = sum(1 for i in range(0, len(px), 2) if px[i:i + 2] == WHITE)
rows_with_text = {i // 1280 for i in range(0, len(px), 2) if px[i:i + 2] == WHITE}
check('show_draws_message', r.returncode == 0 and len(px) == 614400 and corner != bytes([0, 0])
      and white > 2000 and len(rows_with_text) > 64, f'rc={r.returncode} white={white} rows={len(rows_with_text)} {r.stdout}')


# ---- real loop devices (x86 build, needs sudo) ----
def ext4_image(path, mib, files=None, dirty=False):
    with open(path, 'wb') as f:
        f.truncate(mib * 1024 * 1024)
    subprocess.run(['mkfs.ext4', '-q', '-F', str(path)], check=True)
    for name, data in (files or {}).items():
        local = tmp / 'payload'
        local.write_bytes(data)
        subprocess.run(['debugfs', '-w', '-R', f'write {local} {name}', str(path)], check=True, capture_output=True)
    if dirty:                                       # "not cleanly unmounted": e2fsck -p must check it
        subprocess.run(['debugfs', '-w', '-R', 'ssv state 0', str(path)], check=True, capture_output=True)
    return path.read_bytes()


def sudo(*args):
    return subprocess.run(['sudo', '-n', *map(str, args)], capture_output=True, text=True)


if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode != 0:
    print('SKIP attach_real_loops, attach_uses_direct_io, fatroot_dirty_ext4_is_repaired (no sudo)')
else:
    root_img = ext4_image(tmp / 'nzroot.img', 32, files={'os-release': b'PRETTY_NAME="Debian GNU/Linux 12"\n'},
                          dirty=True)
    data_img = ext4_image(tmp / 'nzdata.img', 16)
    real = make_fake_card(tmp / 'real.img', size_mib=128, part_mib=112,
                          files={'cubegm/Bubbles.scr': b'dtb', 'cubegm/advapi32.dll': b'k',
                                 'nzeus/nzroot.img': root_img, 'nzeus/nzdata.img': data_img})
    d = Path(tempfile.mkdtemp(prefix='real-', dir=tmp))
    env = ['env', f'NZ_ROOT={d}', 'NZ_ROOT_IMG_SIZE=33554432', 'NZ_E2FSCK=/usr/sbin/e2fsck']
    r = sudo(*env, x86, 'attach', real, '/newroot')
    ok = (r.returncode == 0 and (d / 'newroot/os-release').exists()
          and os.path.ismount(d / 'data0') and (d / 'fat/nzeus/nzroot.img').exists())
    check('attach_real_loops', ok, r.stdout + r.stderr)
    # all three loop devices use direct I/O: the data comes by DMA, never a CPU copy (on the
    # console's aliasing data cache a CPU copy without a flush gave programs stale bytes)
    want = {str(real), str(d / 'fat/nzeus/nzroot.img'), str(d / 'fat/nzeus/nzdata.img')}
    rows = [l.split() for l in sudo('losetup', '-l', '-n', '--raw', '-O', 'BACK-FILE,DIO').stdout.splitlines()]
    dio = {row[0]: row[-1] for row in rows if row and row[0] in want}      # backing file -> "1" = direct I/O
    said = all(f'direct-io {x}: on' in r.stdout for x in ('fat', 'root', 'data'))
    check('attach_uses_direct_io', said and len(dio) == 3 and set(dio.values()) == {'1'}, f'{dio} {r.stdout}')
    # the root image was marked dirty: after attach, e2fsck must have cleaned it
    loops = sudo('losetup', '-j', str(d / 'fat/nzeus/nzroot.img')).stdout
    check('fatroot_dirty_ext4_is_repaired', 'fsck root: repaired' in r.stdout,
          r.stdout + loops)
    r = sudo(*env, x86, 'detach', '/newroot')
    check('detach_undoes_everything', r.returncode == 0 and not os.path.ismount(d / 'newroot')
          and not os.path.ismount(d / 'fat') and not os.path.ismount(d / 'data0')
          and sudo('losetup', '-j', str(real)).stdout.strip() == '', r.stdout + r.stderr)

    # a damaged nzdata.img (Windows users can't repair ext4): NZEUS still starts, with a
    # temporary empty /data and a warning, instead of stopping in the rescue shell
    bad_data = bytearray(data_img)
    bad_data[1024:2048] = bytes(1024)                     # wipe the ext4 superblock
    card3 = make_fake_card(tmp / 'baddata.img', size_mib=128, part_mib=112,
                           files={'cubegm/Bubbles.scr': b'dtb', 'cubegm/advapi32.dll': b'k',
                                  'nzeus/nzroot.img': root_img, 'nzeus/nzdata.img': bytes(bad_data)})
    d3 = Path(tempfile.mkdtemp(prefix='bad-', dir=tmp))
    env3 = ['env', f'NZ_ROOT={d3}', 'NZ_ROOT_IMG_SIZE=33554432', 'NZ_E2FSCK=/usr/sbin/e2fsck']
    r = sudo(*env3, x86, 'attach', card3, '/newroot')
    fstype = [l.split()[2] for l in Path('/proc/mounts').read_text().splitlines() if l.split()[1] == str(d3 / 'data0')]
    check('data_damaged_boots_with_temporary_data', r.returncode == 0 and fstype == ['tmpfs']
          and 'fatroot-warning: nzdata.img is damaged' in r.stdout and (d3 / 'newroot/os-release').exists(),
          f'{fstype} {r.stdout}')
    sudo(*env3, x86, 'detach', '/newroot')

    # a damaged nzroot.img: the message tells a Windows user what to do
    bad_root = bytearray(root_img)
    bad_root[1024:2048] = bytes(1024)
    card4 = make_fake_card(tmp / 'badroot.img', size_mib=128, part_mib=112,
                           files={'cubegm/Bubbles.scr': b'dtb', 'cubegm/advapi32.dll': b'k',
                                  'nzeus/nzroot.img': bytes(bad_root), 'nzeus/nzdata.img': data_img})
    d4 = Path(tempfile.mkdtemp(prefix='badroot-', dir=tmp))
    r = sudo('env', f'NZ_ROOT={d4}', 'NZ_ROOT_IMG_SIZE=33554432', 'NZ_E2FSCK=/usr/sbin/e2fsck', x86, 'attach', card4, '/newroot')
    check('root_damaged_says_reinstall', r.returncode == 5 and 'install.bat' in r.stdout, r.stdout)

# clean up only when nothing is still mounted below the test folder
if str(tmp) not in Path('/proc/mounts').read_text():
    shutil.rmtree(tmp, ignore_errors=True)
else:
    print(f'NOTE left {tmp} in place: something is still mounted there')
total = 16
print(f'{total - len(failures)}/{total} passed' if not failures else f'FAILED: {failures}')
sys.exit(1 if failures else 0)
