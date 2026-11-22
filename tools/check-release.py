#!/usr/bin/env python3
"""Checks a NZEUS Deck release zip before anyone downloads it (run in WSL).

  python3 tools/check-release.py dist/NZEUS-Deck-<version>.zip
  python3 tools/check-release.py --self-test        (proves the checks catch a bad zip)

Env:
  NZ_BOOTLOADER     my factory bootloader file: gives the wrapper key, which
                    must appear in NO file of the zip
  NZ_STOCK_CUBEGM   folder with the stock card's own cubegm/ files (the maker's):
                    no zip file may be identical to one of them
  NZ_PRIVATE_WORDS_DIR  my private word lists (privacy scan of nzroot.img)
"""
import hashlib
import io
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'linux-port' / 'boot-analysis'))
from image_codec import factory_key  # noqa: E402

TOP = None                       # set from the zip: NZEUS-Deck-<version>/
SUMS = 'more/SHA256SUMS'
EXPECTED = ['READ ME FIRST.txt', 'install.bat', 'install-new-card.bat', 'uninstall.bat', 'collect-logs.bat',
            'more/installer/NzeusCard.psm1', 'more/installer/install.ps1', 'more/installer/uninstall.ps1',
            'more/installer/collect-logs.ps1', 'more/installer/new-card.ps1', 'more/r36s_card.py', 'more/INSTALL.md',
            'more/SOURCE.txt', 'more/LICENSE', SUMS, 'card/cubegm/advapi32.dll', 'card/cubegm/ApplicationFrame.dll',
            'card/cubegm/xgame-logo.bmp',
            'card/nzeus/nzroot.img', 'card/nzeus/nzdata.img', 'card/nzeus/VERSION']
MAX_ZIP = 250 * 1024 * 1024


def sha_stream(f):
    h = hashlib.sha256()
    for block in iter(lambda: f.read(1 << 20), b''):
        h.update(block)
    return h.hexdigest()


def contains(f, needle):
    """Does the stream contain `needle`? (reads in blocks; overlap so a split match is still found)"""
    tail = b''
    for block in iter(lambda: f.read(1 << 22), b''):
        if needle in tail + block:
            return True
        tail = block[-(len(needle) - 1):]
    return False


def check_zip(path, key=None, maker_hashes=(), words_dir=None, quiet=False, allow_skip=False):
    """Returns the list of failed check names. A skipped check counts as failed unless allow_skip."""
    failed, skipped = [], []

    def check(name, ok, detail=''):
        if not quiet:
            print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or not detail else f'  ({detail})'), flush=True)
        if not ok:
            failed.append(name)

    z = zipfile.ZipFile(path)
    names = [n for n in z.namelist() if not n.endswith('/')]
    tops = {n.split('/', 1)[0] for n in names}
    top = tops.pop() if len(tops) == 1 else ''
    rel = sorted(n.split('/', 1)[1] for n in names if '/' in n)
    check('exact_file_list', top.startswith('NZEUS-Deck-') and rel == sorted(EXPECTED),
          f'extra: {sorted(set(rel) - set(EXPECTED))} missing: {sorted(set(EXPECTED) - set(rel))}')

    sums = {}
    if f'{top}/{SUMS}' in names:
        for line in z.read(f'{top}/{SUMS}').decode().splitlines():
            h, n = line.split(None, 1)
            sums[n.lstrip('*')] = h
    hashes = {n: sha_stream(z.open(n)) for n in names}
    bad = [n for n, h in sums.items() if hashes.get(f'{top}/{n}') != h]
    check('sha256sums_match', sums and not bad and set(sums) == set(EXPECTED) - {SUMS}, ', '.join(bad))

    if words_dir and f'{top}/card/nzeus/nzroot.img' in names:
        with tempfile.TemporaryDirectory(prefix='nzpub-release-') as t:
            img = Path(t) / 'nzroot.img'
            with z.open(f'{top}/card/nzeus/nzroot.img') as src, open(img, 'wb') as dst:
                for block in iter(lambda: src.read(1 << 22), b''):
                    if block.count(0) == len(block):
                        dst.seek(len(block), os.SEEK_CUR)         # keep it sparse
                    else:
                        dst.write(block)
                dst.truncate()
            tree = Path(t) / 'root'
            tree.mkdir()
            subprocess.run(['debugfs', '-R', f'rdump / {tree}', str(img)], capture_output=True)
            r = subprocess.run(['bash', str(REPO / 'linux-port/rootfs/debian/privacy-scan-image.sh'), str(tree)],
                               capture_output=True, text=True, env={**os.environ, 'NZ_PRIVATE_WORDS_DIR': words_dir})
            check('images_privacy_scan_clean', r.returncode == 0 and (tree / 'etc').is_dir(),
                  ' '.join(l for l in r.stdout.splitlines() if l.startswith('FAIL')))
    else:
        skipped.append('images_privacy_scan_clean (no NZ_PRIVATE_WORDS_DIR)')

    if maker_hashes:
        same = [n for n, h in hashes.items() if h in maker_hashes]
        check('no_maker_files', not same, ', '.join(same))
    else:
        skipped.append('no_maker_files (no NZ_STOCK_CUBEGM)')

    if key:
        leaks = [n for n in names if contains(z.open(n), key)]
        check('no_nor_dump_or_key', not leaks and not any(n.lower().endswith(('mtd0.bin', 'bootloader.bin'))
                                                          for n in names), ', '.join(leaks))
    else:
        skipped.append('no_nor_dump_or_key (no NZ_BOOTLOADER)')

    size = os.path.getsize(path)
    check('zip_size_under_250MB', size < MAX_ZIP, f'{size / 2**20:.0f} MB')
    for x in skipped:
        if not quiet:
            print('SKIP ' + x + ('' if allow_skip else '  - a skipped check is NOT a pass'))
        if not allow_skip:
            failed.append('skipped ' + x)
    return failed


def self_test():
    """A good zip passes; a zip with an extra file and the key hidden in it is caught."""
    key = bytes(range(32))
    with tempfile.TemporaryDirectory() as t:
        def make(path, extra, after=None):
            files = {n: (b'x' * 10 if not n.endswith('SHA256SUMS') else b'') for n in EXPECTED}
            files.update(extra)
            files[SUMS] = '\n'.join(f'{hashlib.sha256(v).hexdigest()}  {n}' for n, v in files.items()
                                            if n in EXPECTED and n != SUMS).encode()
            files.update(after or {})                      # changed AFTER the sums were written
            with zipfile.ZipFile(path, 'w') as z:
                for n, v in files.items():
                    z.writestr(f'NZEUS-Deck-test/{n}', v)
        good, bad = Path(t) / 'good.zip', Path(t) / 'bad.zip'
        make(good, {})
        make(bad, {'secret.txt': b'...' + key + b'...', 'card/cubegm/advapi32.dll': b'maker kernel'},
             after={'card/nzeus/VERSION': b'tampered'})
        maker = {hashlib.sha256(b'maker kernel').hexdigest()}
        g = check_zip(good, key, maker, quiet=True, allow_skip=True)
        b = check_zip(bad, key, maker, quiet=True, allow_skip=True)
        s = check_zip(good, None, set(), quiet=True)            # nothing to check the key/maker files with
    ok = (not g and {'exact_file_list', 'no_nor_dump_or_key', 'no_maker_files', 'sha256sums_match'} <= set(b)
          and any(x.startswith('skipped') for x in s))
    print(('PASS' if ok else 'FAIL') + f' self_test (good zip failed: {g}; bad zip caught: {b}; '
          f'skipped checks count as failed: {s})')
    return ok


def main():
    if sys.argv[1:] == ['--self-test']:
        sys.exit(0 if self_test() else 1)
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    key = factory_key(Path(os.environ['NZ_BOOTLOADER']).read_bytes()) if os.environ.get('NZ_BOOTLOADER') else None
    maker = set()
    if os.environ.get('NZ_STOCK_CUBEGM'):
        maker = {hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(os.environ['NZ_STOCK_CUBEGM']).iterdir()
                 if p.is_file()}
    failed = check_zip(sys.argv[1], key, maker, os.environ.get('NZ_PRIVATE_WORDS_DIR'))   # skips fail
    print('RELEASE OK' if not failed else f'RELEASE NOT OK: {failed}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
