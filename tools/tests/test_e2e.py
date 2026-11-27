#!/usr/bin/env python3
"""End-to-end on a FAKE card with the REAL release files (WSL, needs sudo for loop devices):

  python3 tools/tests/test_e2e.py dist/NZEUS-Deck-<version>.zip

1. a 6 GiB sparse stock-like card (scrambled sector 0, FAT32 with cubegm/ + a game)
2. install the Mac/Linux way: r36s_card.py open -> copy (backup first) -> close
3. boot: the PC build of nz-fatroot attaches the card exactly like the console's
   boot RAM disk does -> the Debian system and /data are mounted and readable
4. uninstall: open -> stock files back -> delete nzroot.img/nzdata.img/VERSION -> close
5. the card's sector 0 and cubegm/ files are exactly the starting ones again
Never touches a real card.
"""
import hashlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / 'tools'))
import r36s_card  # noqa: E402
from fakecard import make_fake_card  # noqa: E402

failures = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or not detail else f'  ({detail})'), flush=True)
    if not ok:
        failures.append(name)


def run(*a, **k):
    return subprocess.run([str(x) for x in a], capture_output=True, text=True, **k)


def mcopy(img_off, src, dst):
    return run('mcopy', '-o', '-i', img_off, src, '::' + dst)


zpath = Path(sys.argv[1]).resolve()
tmp = Path(tempfile.mkdtemp(prefix='nzpub-e2e-', dir='/tmp'))
z = zipfile.ZipFile(zpath)
top = z.namelist()[0].split('/')[0]
rel = tmp / 'rel'
z.extractall(rel)
rel = rel / top
stock = {'cubegm/advapi32.dll': b'stock kernel ' * 100, 'cubegm/ApplicationFrame.dll': b'stock avp ' * 50,
         'cubegm/Bubbles.scr': b'stock dtb ' * 80, 'games/game.nes': b'\x01\x02\x03' * 1000}
stock['cubegm/xgame-logo.bmp'] = b'stock start-up picture ' * 40     # shown at power-on
card = make_fake_card(tmp / 'card.img', size_mib=6144, part_mib=6000, files=stock)
start_sector0 = open(card, 'rb').read(512)
fat = f'{card}@@{2048 * 512}'

# 2. install (Mac/Linux steps of INSTALL.md)
r1 = run(sys.executable, REPO / 'tools/r36s_card.py', 'open', card)
for n in ('advapi32.dll', 'ApplicationFrame.dll', 'xgame-logo.bmp'):           # backup first
    run('mmd', '-D', 's', '-i', fat, '::nzeus')
    run('mmd', '-D', 's', '-i', fat, '::nzeus/backup')
    out = tmp / n
    run('mcopy', '-i', fat, f'::cubegm/{n}', out)
    mcopy(fat, out, f'nzeus/backup/{n}')
copies = [mcopy(fat, rel / 'card' / p, p) for p in ('cubegm/advapi32.dll', 'cubegm/ApplicationFrame.dll',
                                                     'cubegm/xgame-logo.bmp',
                                                     'nzeus/nzroot.img', 'nzeus/nzdata.img', 'nzeus/VERSION')]
r2 = run(sys.executable, REPO / 'tools/r36s_card.py', 'close', card)
check('e2e_install_by_hand', r1.returncode == 0 and r2.returncode == 0 and all(c.returncode == 0 for c in copies)
      and r36s_card.inspect_image(card)['state'] == 'closed', ' '.join(c.stderr for c in copies))

# 3. boot: nz-fatroot (PC build) on the closed card, as the console does
x86 = tmp / 'nz-fatroot'
run('gcc', '-O2', '-o', x86, REPO / 'ports/r36s-hichip/fatroot/nz-fatroot.c')
root = tmp / 'root'
root.mkdir()
a = run('sudo', '-n', 'env', f'NZ_ROOT={root}', 'NZ_E2FSCK=/usr/sbin/e2fsck', x86, 'attach', card, '/newroot')
osr = run('sudo', '-n', 'cat', root / 'newroot/etc/os-release').stdout
deck = run('sudo', '-n', 'test', '-x', root / 'newroot/usr/local/bin/nzeus-modern').returncode == 0
data = run('sudo', '-n', 'ls', root / 'data0').stdout.split()
check('e2e_boot_helper_mounts_release', a.returncode == 0 and 'bookworm' in osr and deck and 'nzeus' in data,
      a.stdout + a.stderr + osr[:80])
d = run('sudo', '-n', 'env', f'NZ_ROOT={root}', x86, 'detach', '/newroot')
check('e2e_detach_clean', d.returncode == 0 and str(root) not in Path('/proc/mounts').read_text())

# 4. uninstall by hand
run(sys.executable, REPO / 'tools/r36s_card.py', 'open', card)
for n in ('advapi32.dll', 'ApplicationFrame.dll', 'xgame-logo.bmp'):
    out = tmp / ('b-' + n)
    run('mcopy', '-o', '-i', fat, f'::nzeus/backup/{n}', out)
    mcopy(fat, out, f'cubegm/{n}')
for p in ('nzeus/nzroot.img', 'nzeus/nzdata.img', 'nzeus/VERSION'):
    run('mdel', '-i', fat, '::' + p)
run(sys.executable, REPO / 'tools/r36s_card.py', 'close', card)

# 5. the stock state is back
back = {}
for p in stock:
    out = tmp / ('s-' + p.replace('/', '_'))
    run('mcopy', '-o', '-i', fat, '::' + p, out)
    back[p] = out.read_bytes() if out.exists() else b''
check('e2e_uninstall_restores_stock', back == stock and open(card, 'rb').read(512) == start_sector0
      and r36s_card.inspect_image(card)['state'] == 'closed')

if str(tmp) not in Path('/proc/mounts').read_text():
    shutil.rmtree(tmp, ignore_errors=True)
print('4/4 passed' if not failures else f'FAILED: {failures}')
sys.exit(1 if failures else 0)
