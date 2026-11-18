#!/usr/bin/env python3
"""Tests of the public kernel + AVP stand-in made by build.sh and package.py (run in WSL):

  python3 linux-port/kernel-public/test-package.py --out DIR --build BUILD_DIR --bootloader FACTORY_BOOTLOADER.bin
                                                  [--daily-dts DAILY_board.dts]

DIR = package.py's output, BUILD_DIR = build.sh's kernel build folder (its .config),
FACTORY_BOOTLOADER = my extracted factory bootloader (gives the wrapper key; never copied).
"""
import argparse
import gzip
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'boot-analysis'))
from image_codec import check_uimage, decode, factory_key  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--build', type=Path, required=True)
ap.add_argument('--bootloader', type=Path, required=True)
ap.add_argument('--daily-dts', type=Path)
a = ap.parse_args()
key = factory_key(a.bootloader.read_bytes())
failures, total = [], 0


def check(name, ok, detail=''):
    global total
    total += 1
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or not detail else f'  ({detail})'), flush=True)
    if not ok:
        failures.append(name)


def unwrap(path):
    """advapi32.dll / ApplicationFrame.dll -> (uImage info, payload)."""
    plain = decode(path.read_bytes(), key)
    return check_uimage(plain), plain[64:]


def dtb_bootargs(dtb):
    with tempfile.NamedTemporaryFile(suffix='.dtb') as f:
        f.write(dtb)
        f.flush()
        return subprocess.run(['fdtget', '-t', 's', f.name, '/chosen', 'bootargs'],
                              capture_output=True, text=True).stdout.strip()


# 1. the board description is the daily one minus the blkdevparts= token (when the daily is given)
pub = (HERE / 'board.dts').read_text().splitlines()
if a.daily_dts:
    daily = a.daily_dts.read_text().splitlines()
    diff = [(x, y) for x, y in zip(daily, pub) if x != y]
    ok = (len(daily) == len(pub) and len(diff) == 1 and 'bootargs' in diff[0][0]
          and diff[0][1].replace('"', '').rstrip(';').split() ==
          [t for t in diff[0][0].replace('"', '').rstrip(';').split() if not t.startswith('blkdevparts=')])
    check('dts_differs_only_by_blkdevparts', ok, str(diff[:2]))
else:
    print('SKIP dts_differs_only_by_blkdevparts (no --daily-dts)')

# 2. kernel settings: the daily kernel's, only the RAM disk and the version name differ
daily_cfg = set(l for l in (HERE / 'kernel.config').read_text().splitlines() if l and not l.startswith('#'))
built_cfg = set(l for l in (a.build / '.config').read_text().splitlines() if l and not l.startswith('#'))
changed = sorted(l.split('=')[0] for l in daily_cfg ^ built_cfg)
check('config_differs_only_by_initramfs_and_localversion',
      set(changed) <= {'CONFIG_INITRAMFS_SOURCE', 'CONFIG_LOCALVERSION'} and 'CONFIG_LOCALVERSION="-nzeus-public"' in built_cfg,
      ', '.join(changed))

# 3. the kernel wrapper opens with the factory key; header + payload CRCs right; the DTB is ours
kinfo, kpay = unwrap(a.out / 'advapi32.dll')
kernel = gzip.decompress(kpay)
# the appended board description: the first FDT magic that really has /chosen/bootargs
dtb_at, bootargs = kernel.find(bytes.fromhex('d00dfeed')), ''
while dtb_at >= 0 and not bootargs:
    bootargs = dtb_bootargs(kernel[dtb_at:dtb_at + 65536])
    if not bootargs:
        dtb_at = kernel.find(bytes.fromhex('d00dfeed'), dtb_at + 4)
check('kernel_wrapper_decodes_and_crc_ok', kinfo['arch'] == 5 and dtb_at > 0 and b'-nzeus-public' in kernel, str(kinfo))

# 4. the AVP stand-in: 5 instructions, STANDALONE uImage, CRCs right
ainfo, apay = unwrap(a.out / 'ApplicationFrame.dll')
check('avp_stub_wrapper_decodes_and_crc_ok', (ainfo['os'], ainfo['type']) == (17, 1)
      and apay[:4] == bytes.fromhex('80b8083c'), str(ainfo))

# 5. the key itself is nowhere in what we ship or log
leaks = [p.name for p in a.out.rglob('*') if p.is_file() and key in p.read_bytes()]
check('no_key_bytes_in_outputs', not leaks and len(list(a.out.iterdir())) > 0, ', '.join(leaks))

# 6. the command line: no card layout of mine, the 900 MHz clock kept
check('bootargs_has_no_blkdevparts', bootargs != '' and 'blkdevparts=' not in bootargs and 'nzclk=900' in bootargs,
      bootargs)

# 7. the built-in RAM disk is the copy-and-go one (nz-fatroot + e2fsck inside)
cpio = b''
pos = kernel.find(b'\x1f\x8b\x08', 0x900000)
while pos != -1 and not cpio.startswith(b'070701'):
    try:
        cpio = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(kernel[pos:pos + 8_000_000])
    except zlib.error:
        cpio = b''
    pos = kernel.find(b'\x1f\x8b\x08', pos + 1)
check('initramfs_is_fatroot', b'bin/nz-fatroot' in cpio and b'bin/e2fsck' in cpio and b'bin/nz-sdboot' not in cpio)

print(f'{total - len(failures)}/{total} passed' if not failures else f'FAILED: {failures}')
sys.exit(1 if failures else 0)
