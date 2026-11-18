#!/usr/bin/env python3
"""Open or close an R36S (HiChip) SD card so a PC can use it.

The console keeps its card's partition table (sector 0) scrambled: every 4
bytes XOR EF BE AD DE. A PC then sees an empty disk. "open" unscrambles
sector 0 so the PC shows the card as a normal FAT32 drive; "close" scrambles
it again, so the card goes back to the console exactly as it was.
Only sector 0 is ever written, and it is read back after the write.

  sudo python3 r36s_card.py status /dev/sdX      (or an image file)
  sudo python3 r36s_card.py open   /dev/sdX
  sudo python3 r36s_card.py close  /dev/sdX

Exit codes: 0 done (or already like that), 2 not an R36S card, 1 other error.
Same rules as installer/NzeusCard.psm1 (Windows) and nz-fatroot (console).
"""
import os
import struct
import sys

SECTOR = 512
KEY = bytes([0xEF, 0xBE, 0xAD, 0xDE])          # DEADBEEF as stored (little-endian)


class NotR36sCard(Exception):
    """The disk isn't an R36S card this tool knows (the reason is the message)."""


def scramble(sector):
    """The factory scramble. Doing it twice gives the original back."""
    return bytes(b ^ KEY[i % 4] for i, b in enumerate(sector))


def _u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def check_table(mbr, read_at, size):
    """Is this (plain) sector 0 an R36S card's partition table?
    read_at(offset, count) -> bytes. Returns (start_lba, sectors, type) or raises NotR36sCard."""
    if mbr[510:512] != b'\x55\xaa':
        raise NotR36sCard('no partition-table mark')
    # (bytes 0-439 may hold start-up code: the 16 GB stock card has some, the 64 GB one doesn't)
    ptype, start, count = mbr[446 + 4], _u32(mbr, 446 + 8), _u32(mbr, 446 + 12)
    if ptype not in (0x0B, 0x0C):
        raise NotR36sCard(f'the first partition is not FAT32 (type {ptype})')
    if any(mbr[446 + 16:510]):
        raise NotR36sCard('more than one partition')
    if start < 1 or count < 1:
        raise NotR36sCard('an empty partition')
    if (start + count) * SECTOR > size:
        raise NotR36sCard('the partition is bigger than the card')
    boot = read_at(start * SECTOR, SECTOR)
    if boot[510:512] != b'\x55\xaa' or boot[0x52:0x5A] != b'FAT32   ':
        raise NotR36sCard('no FAT32 file system where the partition starts')
    if _u32(boot, 0x1C) != start or _u32(boot, 0x20) != count:
        raise NotR36sCard("the file system doesn't match the partition table")
    # the console's CUBEGM folder must be in the top folder (first cluster of the root)
    bps, spc, reserved, fats = _u16(boot, 11), boot[13], _u16(boot, 14), boot[16]
    fat_size, root_cluster = _u32(boot, 36), _u32(boot, 44)
    if bps != SECTOR or spc < 1:
        raise NotR36sCard('an unusual FAT32 layout')
    root_at = (start + reserved + fats * fat_size + (root_cluster - 2) * spc) * SECTOR
    root = read_at(root_at, spc * SECTOR)
    for e in range(0, len(root), 32):
        if root[e] == 0:
            break
        if root[e:e + 11] == b'CUBEGM     ' and root[e + 11] & 0x10:
            return start, count, ptype
    raise NotR36sCard('no CUBEGM folder on it (not an R36S card)')


def _size(f):
    f.seek(0, os.SEEK_END)
    return f.tell()


def _read_at(f):
    def read_at(offset, count):
        f.seek(offset)
        data = f.read(count)
        if len(data) != count:
            raise NotR36sCard(f'could not read {count} bytes at {offset}')
        return data
    return read_at


def inspect_file(f):
    """State of an open card/image file: {'state': 'open'|'closed', 'start_lba', 'sectors', 'type'}."""
    size, read_at = _size(f), _read_at(f)
    sector0 = read_at(0, SECTOR)
    try:
        start, count, ptype = check_table(sector0, read_at, size)
        state = 'open'
    except NotR36sCard as why_open:
        try:
            start, count, ptype = check_table(scramble(sector0), read_at, size)
            state = 'closed'
        except NotR36sCard as why_closed:
            raise NotR36sCard(f'as it is: {why_open}; unscrambled: {why_closed}')
    return {'state': state, 'start_lba': start, 'sectors': count, 'type': ptype}


def inspect_image(path):
    with open(path, 'rb') as f:
        return inspect_file(f)


def set_state(path, want):
    """Open or close the card at path. Returns what was done: 'opened', 'closed' or 'nothing'."""
    with open(path, 'r+b', buffering=0) as f:
        info = inspect_file(f)
        if info['state'] == want:
            return 'nothing'
        f.seek(0)
        new = scramble(f.read(SECTOR))                 # open <-> closed is the same XOR
        f.seek(0)
        f.write(new)
        os.fsync(f.fileno())
        f.seek(0)
        if f.read(SECTOR) != new:
            raise OSError('sector 0 read back differently: the card may be failing')
        if inspect_file(f)['state'] != want:
            raise OSError('the card is not in the new state after writing')
    return 'opened' if want == 'open' else 'closed'


def main(argv):
    if len(argv) != 3 or argv[1] not in ('status', 'open', 'close'):
        print(__doc__)
        return 1
    action, path = argv[1], argv[2]
    try:
        if action == 'status':
            info = inspect_image(path)
            print(f"{path}: R36S card, {info['state'].upper()} "
                  f"(FAT32 at sector {info['start_lba']}, {info['sectors'] * SECTOR / 2**30:.1f} GiB)")
        else:
            done = set_state(path, 'open' if action == 'open' else 'closed')
            print(f'{path}: ' + ('already ' + action + ('ed' if action == 'open' else 'd')
                                 if done == 'nothing' else done))
        return 0
    except NotR36sCard as e:
        print(f'{path}: not an R36S card this tool knows ({e}). Nothing was changed.')
        return 2
    except OSError as e:
        print(f'{path}: {e}')
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
