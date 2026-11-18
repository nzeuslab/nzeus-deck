#!/usr/bin/env python3
"""Tests for tools/r36s_card.py. Run in WSL: python3 tools/tests/test_r36s_card.py"""
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
from fakecard import make_fake_card, xor_sector  # noqa: E402

failures = []


def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name, flush=True)
    if not cond:
        failures.append(name)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def cli(*args):
    return subprocess.run([sys.executable, str(HERE.parent / 'r36s_card.py'), *map(str, args)],
                          capture_output=True, text=True)


import r36s_card  # noqa: E402  (fails here until the tool exists: that's the RED step)

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)

    # scramble is its own inverse and matches the factory XOR
    s = bytes(range(256)) * 2
    check('test_scramble_roundtrip',
          r36s_card.scramble(r36s_card.scramble(s)) == s and r36s_card.scramble(s) == xor_sector(s))

    closed = make_fake_card(tmp / 'closed.img')
    info = r36s_card.inspect_image(closed)
    check('test_inspect_closed', info['state'] == 'closed' and info['start_lba'] == 2048
          and info['sectors'] == 48 * 2048 and info['type'] == 0x0C)

    opened = make_fake_card(tmp / 'open.img', scrambled=False)
    check('test_inspect_open', r36s_card.inspect_image(opened)['state'] == 'open')

    two = make_fake_card(tmp / 'two.img', extra_partition=True)
    try:
        r36s_card.inspect_image(two)
        check('test_rejects_two_partitions', False)
    except r36s_card.NotR36sCard:
        check('test_rejects_two_partitions', True)

    bad = make_fake_card(tmp / 'bad.img', mbr_count_delta=-8)
    try:
        r36s_card.inspect_image(bad)
        check('test_rejects_size_mismatch', False)
    except r36s_card.NotR36sCard:
        check('test_rejects_size_mismatch', True)

    stick = make_fake_card(tmp / 'stick.img', files={'photos/a.jpg': b'x'})
    try:
        r36s_card.inspect_image(stick)
        check('test_rejects_no_cubegm', False)
    except r36s_card.NotR36sCard:
        check('test_rejects_no_cubegm', True)

    real16 = make_fake_card(tmp / 'real16.img', boot_code=True, start_lba=32)
    try:
        info = r36s_card.inspect_image(real16)
        check('test_accepts_card_with_boot_code', info['state'] == 'closed' and info['start_lba'] == 32)
    except r36s_card.NotR36sCard as e:
        check('test_accepts_card_with_boot_code', False)
        print('   ', e)

    card = make_fake_card(tmp / 'card.img')
    before = sha(card)
    r1, r2 = cli('open', card), cli('close', card)
    check('test_open_then_close_is_byte_identical',
          r1.returncode == 0 and r2.returncode == 0 and sha(card) == before)

    cli('open', card)
    once = sha(card)
    r = cli('open', card)
    check('test_open_twice_is_noop', r.returncode == 0 and sha(card) == once)

    r = cli('status', two)
    check('test_cli_not_r36s_exit_2', r.returncode == 2)

print(f'{10 - len(failures)}/10 passed')
sys.exit(1 if failures else 0)
