"""Makes small fake R36S SD-card images for tests (run in WSL: needs mkfs.vfat + mtools).

A real stock card: an MBR with ONE FAT32 partition at LBA 2048, and sector 0
scrambled (every 4 bytes XOR EF BE AD DE) so Windows sees an empty disk.
"""
import os
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR = 512
START_LBA = 2048
XOR = bytes([0xEF, 0xBE, 0xAD, 0xDE])


def xor_sector(sector):
    """The factory scramble (the same step both ways)."""
    return bytes(b ^ XOR[i % 4] for i, b in enumerate(sector))


def mbr(parts):
    """parts = [(type, start_lba, sectors), ...] -> a 512-byte MBR."""
    m = bytearray(SECTOR)
    struct.pack_into('<I', m, 440, 0x1234ABCD)          # disk signature, like a real card
    for i, (ptype, start, count) in enumerate(parts):
        e = 446 + 16 * i
        m[e + 4] = ptype
        struct.pack_into('<II', m, e + 8, start, count)
    m[510:512] = b'\x55\xaa'
    return bytes(m)


def make_fake_card(path, size_mib=64, part_mib=48, scrambled=True, files=None,
                   extra_partition=False, mbr_count_delta=0, boot_code=False, start_lba=None):
    """Write a fake card image to `path` and return its path.

    files: {'cubegm/advapi32.dll': b'...', ...} copied into the FAT32.
    extra_partition: add a second partition entry (not an R36S card).
    mbr_count_delta: make the MBR's sector count disagree with the FAT boot sector.
    boot_code: fill sector 0's first 440 bytes like a real 16 GB stock card does.
    start_lba: where the FAT32 starts (stock cards: 2048, or 32 on the 16 GB one).
    """
    path = Path(path)
    start = START_LBA if start_lba is None else start_lba
    if files is None:                                   # what a stock card has in cubegm/
        files = {'cubegm/advapi32.dll': b'stock kernel', 'cubegm/Bubbles.scr': b'stock dtb',
                 'cubegm/ApplicationFrame.dll': b'stock avp'}
    count = part_mib * 1024 * 1024 // SECTOR
    with tempfile.TemporaryDirectory() as tmp:
        part = Path(tmp) / 'part.img'
        subprocess.run(['mkfs.vfat', '-F', '32', '-s', '1', '-n', 'NO NAME', '-h', str(start),
                        '-C', str(part), str(count * SECTOR // 1024)],
                       check=True, capture_output=True)
        for name, data in files.items():
            src = Path(tmp) / 'f'
            src.write_bytes(data)
            folder = os.path.dirname(name)
            if folder:
                subprocess.run(['mmd', '-D', 's', '-i', str(part), '::' + folder],
                               capture_output=True)   # "already there" is fine
            subprocess.run(['mcopy', '-o', '-i', str(part), str(src), '::' + name],
                           check=True, capture_output=True)
        # mkfs.vfat may round the size down (track geometry): a real card's table matches its FAT32
        count = struct.unpack_from('<I', part.read_bytes()[:512], 0x20)[0]
        parts = [(0x0C, start, count + mbr_count_delta)]
        if extra_partition:
            parts.append((0x83, start + count, 2048))
        sector0 = mbr(parts)
        if boot_code:
            sector0 = bytes((i * 7 + 3) & 255 for i in range(440)) + sector0[440:]
        if scrambled:
            sector0 = xor_sector(sector0)
        with open(path, 'wb') as f:
            f.truncate(size_mib * 1024 * 1024)
            f.write(sector0)
            f.seek(start * SECTOR)
            f.write(part.read_bytes())
    return path
