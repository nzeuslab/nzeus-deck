#!/usr/bin/env python3
"""NZEUS Linux area ("region") planner for the R36S test card. PC side only.

THIS TOOL NEVER OPENS HARDWARE AND HAS NO INSTALL ACTION. It only reads and
writes ordinary image files (it refuses /dev/*, \\\\.\\ paths and anything that
is not a regular file), like linux-port/card-test/prepare_card.py. The real card
is read and written only by linux-port/card-test/card_io.py (region-* actions,
elevated, Windows), whose rules live in card-test/region_io.py - the same module
this tool's selftest runs against a fake card file.

What it does (see ../PLAN.md for the why):
  layout    compute the Linux area from the card size and the end of the stock
            FAT32 partition; print the kernel's blkdevparts= line
  inspect   read a card SNAPSHOT image (the usual 16357785600-byte before.img):
            unscramble sector 0 (XOR DEADBEEF), check the single FAT32
            partition and its boot sector, and print the resulting layout
  survey    read the Linux area of a full card IMAGE FILE (fake card) with the
            card_io.py survey code (region-survey.json + region-before.blocks)
  build     make the candidate content of the whole area as a sparse file:
            NZEUS header + ext4 "nzroot" + ext4 "nzdata" + backup header
  plan      compare candidate with survey -> delta.json in the same style as
            prepare_card.py (absolute offsets, 64 KiB blocks, before/after
            SHA-256) + region-after.blocks (the new block contents, in plan
            order), refusing if any block would fall outside the area
  check     re-check a plan folder (delta.json + its data files) against the rules
  selftest  everything above on a FAKE sparse 64 GB card image, plus card_io.py's
            own survey/install/restore/reset/verify code (region_io.py) on
            copies of that fake image, and the failure cases

Python 3.12+, standard library only, plus e2fsprogs (mke2fs/e2fsck/tune2fs/
resize2fs/dumpe2fs) for build and dosfstools/mtools for the selftest.
"""
import argparse
import errno
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

# card_io.py's Linux-area rules (one copy, used by the real card and the selftest).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'card-test'))
import region_io

KiB, MiB, GiB = 1 << 10, 1 << 20, 1 << 30
CARD_BYTES = 63864569856          # the 64 GB test card (card_io.py CARD_SIZE)
SNAPSHOT_BYTES = 16357785600      # what card_io.py snapshots (IMAGE_SIZE)
FAT_START = 1 * MiB               # stock FAT32 partition starts at LBA 2048
ALIGN = 4 * MiB                   # every Linux-area boundary is 4 MiB aligned
REGION_START = 16 * GiB           # first byte of the Linux area (LBA 33554432)
GUARD = 64 * MiB                  # minimum untouched gap after the stock FAT32
HEADER_BYTES = 4 * MiB            # header partitions (one sector used, rest zero)
ROOT_BYTES = 4 * GiB              # ext4 "nzroot"
MIN_DATA_BYTES = 1 * GiB
BLOCK = region_io.BLOCK           # delta granularity, as prepare_card.py (64 KiB)
DEFAULT_MAX_PLAN = region_io.MAX_PLAN_BYTES   # 2 GiB, reasons in region_io.py
DEFAULT_ROOTFS = '~/nzeus-rootfs-debian/out/rootfs.ext4'   # WSL Debian root build
TEST_CARD_CID = '000000000000000000000000000000b7'
HEADER_MAGIC = b'NZEUS-LINUX-AREA'
HEADER_VERSION = 1
ZERO_BLOCK = bytes(BLOCK)
ZERO_SHA = hashlib.sha256(ZERO_BLOCK).hexdigest()
KIND = {'stock-fat32': 0, 'header': 1, 'ext4-root': 2, 'ext4-data': 3}

# One exception type for both modules: a safety rule said no.
Refused = region_io.Refused
refuse = region_io.need
sha = region_io.sha


# ---------------------------------------------------------------- file safety
def image_path(path, must_exist=True):
    """Only ordinary image files: never a disk, a /dev node or a Windows device."""
    text = str(path)
    refuse(not text.startswith('\\\\') and not text.startswith('//./'),
           f'{text}: Windows device paths are not allowed (this tool never opens hardware)')
    p = Path(path)
    refuse(not str(p.resolve()).startswith('/dev/'), f'{text}: /dev paths are not allowed')
    if must_exist:
        refuse(p.is_file(), f'{text}: not a regular file')
    elif p.exists():
        refuse(p.is_file(), f'{text}: exists and is not a regular file')
    return p


def read_at(path, offset, count):
    with open(image_path(path), 'rb') as f:
        f.seek(offset)
        data = f.read(count)
    refuse(len(data) == count, f'{path}: short read at {offset}')
    return data


def data_ranges(f, start, end):
    """(a, b) byte ranges in [start, end) of a regular file that may hold data.
    Holes read as zero, so everything outside these ranges is zero."""
    fd = f.fileno()
    size = os.fstat(fd).st_size
    end = min(end, size)
    pos = start
    while pos < end:
        try:
            d = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError as e:
            if e.errno == errno.ENXIO:
                return
            if e.errno in (errno.EINVAL, errno.EOPNOTSUPP, errno.ENOSYS):
                yield pos, end          # no hole support (e.g. /mnt/d): treat all as data
                return
            raise
        if d >= end:
            return
        h = os.lseek(fd, d, os.SEEK_HOLE)
        yield d, min(h, end)
        pos = h


def blocks_with_data(f, start, end, base=0):
    """BLOCK-aligned (relative to base) block starts that overlap data in [start, end)."""
    seen = set()
    for a, b in data_ranges(f, start, end):
        first = base + (a - base) // BLOCK * BLOCK
        for blk in range(first, b, BLOCK):
            if blk not in seen:
                seen.add(blk)
                yield blk


def sparse_copy(src, dst, dst_offset=0, length=None):
    """Copy a regular file into another one at dst_offset, keeping holes."""
    with open(image_path(src), 'rb') as s, open(dst, 'r+b') as d:
        size = os.fstat(s.fileno()).st_size if length is None else length
        for a, b in data_ranges(s, 0, size):
            s.seek(a)
            d.seek(dst_offset + a)
            left = b - a
            while left:
                chunk = s.read(min(left, 4 * MiB))
                d.write(chunk)
                left -= len(chunk)


def hash_file_prefix(path, length):
    """SHA-256 of the first `length` bytes; holes are hashed as zeros quickly."""
    h = hashlib.sha256()
    zeros = bytes(4 * MiB)
    with open(image_path(path), 'rb') as f:
        refuse(os.fstat(f.fileno()).st_size >= length, f'{path}: shorter than {length}')
        pos = 0
        for a, b in list(data_ranges(f, 0, length)) + [(length, length)]:
            while pos < a:
                n = min(a - pos, len(zeros))
                h.update(zeros[:n])
                pos += n
            f.seek(a)
            while pos < b:
                chunk = f.read(min(b - pos, 4 * MiB))
                h.update(chunk)
                pos += len(chunk)
    return h.hexdigest()


# ------------------------------------------------------------ stock card prefix
def unscramble(sector):
    """Stock R36S cards store sector 0 XOR DEADBEEF (bytes EF BE AD DE)."""
    key = b'\xef\xbe\xad\xde'
    return bytes(b ^ key[i % 4] for i, b in enumerate(sector))


def table_problem(mbr, boot_reader, card_bytes):
    """Same checks as release/r36s_card.ps1 Test-R36sTable. '' = looks right."""
    if mbr[510:512] != b'\x55\xaa':
        return 'no partition-table mark'
    if any(mbr[:440]):
        return 'start-up code in sector 0 (the R36S card has none)'
    ptype = mbr[446 + 4]
    if ptype not in (0x0B, 0x0C):
        return f'the first partition is not FAT32 (type {ptype:#x})'
    if any(mbr[462:510]):
        return 'more than one partition'
    start, count = struct.unpack_from('<II', mbr, 446 + 8)
    if start < 1 or count < 1:
        return 'an empty partition'
    if (start + count) * 512 > card_bytes:
        return 'the partition is bigger than the card'
    boot = boot_reader(start * 512)
    if boot[510:512] != b'\x55\xaa':
        return 'no FAT file system where the partition starts'
    if boot[0x52:0x5A] != b'FAT32   ':
        return 'not FAT32 where the partition starts'
    hidden, total = struct.unpack_from('<II', boot, 0x1C)
    if hidden != start or total != count:
        return "the file system doesn't match the partition table"
    return ''


def parse_prefix(snapshot, card_bytes=CARD_BYTES):
    """Read sector 0 and the FAT32 boot sector from a snapshot image."""
    first_mib = read_at(snapshot, 0, MiB)
    s0 = first_mib[:512]
    size = Path(snapshot).stat().st_size
    reader = lambda off: read_at(snapshot, off, 512) if off + 512 <= size else bytes(512)
    plain_problem = table_problem(s0, reader, card_bytes)
    scrambled_problem = table_problem(unscramble(s0), reader, card_bytes)
    if not scrambled_problem:
        state, mbr = 'closed', unscramble(s0)
    elif not plain_problem:
        state, mbr = 'open', s0
    else:
        raise Refused(f'sector 0 is not an R36S card table (as it is: {plain_problem}; '
                      f'unscrambled: {scrambled_problem})')
    start, count = struct.unpack_from('<II', mbr, 446 + 8)
    boot = read_at(snapshot, start * 512, 512)
    bps, spc, reserved, fats = struct.unpack_from('<HBHB', boot, 11)
    fat_size, root_cluster = struct.unpack_from('<I4xI', boot, 36)
    refuse(bps == 512, f'FAT sector size {bps}, expected 512')
    refuse(start * 512 == FAT_START, f'FAT32 starts at {start * 512}, expected {FAT_START}')
    fat_end = (start + count) * 512
    refuse(fat_end <= size, f'the snapshot ({size} bytes) does not cover the whole FAT32 '
                            f'partition (ends at {fat_end}); take a bigger snapshot first')
    return {'state': state, 'fat_start': start * 512, 'fat_sectors': count, 'fat_end': fat_end,
            'sectors_per_cluster': spc, 'reserved_sectors': reserved, 'fats': fats,
            'fat_size_sectors': fat_size, 'root_cluster': root_cluster,
            'volume_id': f'{struct.unpack_from("<I", boot, 0x43)[0]:08x}',
            'volume_label': boot[0x47:0x52].decode('ascii', 'replace').rstrip(),
            'oem': boot[3:11].decode('ascii', 'replace').rstrip(),
            'first_mib_sha256': sha(first_mib), 'fat_boot_sector_sha256': sha(boot)}


# ---------------------------------------------------------------------- layout
def size_text(n):
    for unit, mult in (('G', GiB), ('M', MiB), ('K', KiB)):
        if n % mult == 0:
            return f'{n // mult}{unit}'
    return str(n)


def compute_layout(card_bytes, fat_end):
    refuse(fat_end % 512 == 0 and fat_end > FAT_START, f'bad FAT end {fat_end}')
    refuse(fat_end + GUARD <= REGION_START,
           f'the stock FAT32 ends at {fat_end}; the Linux area at {REGION_START} needs '
           f'{GUARD} bytes of untouched gap after it')
    region_end = card_bytes // ALIGN * ALIGN
    hdr2 = region_end - HEADER_BYTES
    root = REGION_START + HEADER_BYTES
    data = root + ROOT_BYTES
    refuse(hdr2 - data >= MIN_DATA_BYTES, 'card too small for the Linux area')
    parts = [
        {'num': 1, 'name': 'nzstock', 'kind': 'stock-fat32', 'start': FAT_START, 'bytes': fat_end - FAT_START, 'ro': True},
        {'num': 2, 'name': 'nzhdr', 'kind': 'header', 'start': REGION_START, 'bytes': HEADER_BYTES, 'ro': True},
        {'num': 3, 'name': 'nzroot', 'kind': 'ext4-root', 'start': root, 'bytes': ROOT_BYTES, 'ro': False},
        {'num': 4, 'name': 'nzdata', 'kind': 'ext4-data', 'start': data, 'bytes': hdr2 - data, 'ro': False},
        {'num': 5, 'name': 'nzhdr2', 'kind': 'header', 'start': hdr2, 'bytes': HEADER_BYTES, 'ro': True},
    ]
    for p in parts[1:]:
        refuse(p['start'] % ALIGN == 0 and p['bytes'] % ALIGN == 0, f'{p["name"]} not 4 MiB aligned')
    blk = ','.join(f'{size_text(p["bytes"])}@{size_text(p["start"])}({p["name"]}){"ro" if p["ro"] else ""}'
                   for p in parts)
    return {'version': HEADER_VERSION, 'card_bytes': card_bytes, 'fat_start': FAT_START, 'fat_end': fat_end,
            'snapshot_bytes': SNAPSHOT_BYTES, 'region_start': REGION_START, 'region_end': region_end,
            'untouched_after_fat': [fat_end, REGION_START], 'untouched_card_end': [region_end, card_bytes],
            'parts': parts, 'blkdevparts': 'blkdevparts=mmcblk0:' + blk}


def part(layout, name):
    return next(p for p in layout['parts'] if p['name'] == name)


def fs_uuid(name, cid):
    return str(uuid.UUID(bytes=hashlib.sha256(f'nzeus|{name}|{cid}'.encode()).digest()[:16], version=4))


# ---------------------------------------------------------------------- header
# 512-byte little-endian header in the first sector of nzhdr and nzhdr2:
#   0 magic[16] "NZEUS-LINUX-AREA"   16 u32 version   20 u32 header size (512)
#  24 u64 card bytes   32 u64 FAT end   40 u64 region start   48 u64 region end
#  56 u8 cid[16]   72 u8 first-MiB sha256[32]   104 u32 part count   108 u32 0
# 112 parts, 64 bytes each: name[16] u64 start u64 bytes u32 kind u32 flags(1=ro)
#     u8 fs uuid[16] u8 zero[8]
# 508 u32 CRC-32 (zlib) of bytes 0..507
HEADER_HEAD = struct.Struct('<16sIIQQQQ16s32sII')
HEADER_PART = struct.Struct('<16sQQII16s8x')


def make_header(layout, cid, first_mib_sha256, uuids):
    parts = [p for p in layout['parts']]
    out = bytearray(512)
    HEADER_HEAD.pack_into(out, 0, HEADER_MAGIC, HEADER_VERSION, 512, layout['card_bytes'],
                          layout['fat_end'], layout['region_start'], layout['region_end'],
                          bytes.fromhex(cid), bytes.fromhex(first_mib_sha256), len(parts), 0)
    for i, p in enumerate(parts):
        u = uuid.UUID(uuids[p['name']]).bytes if p['name'] in uuids else bytes(16)
        HEADER_PART.pack_into(out, 112 + 64 * i, p['name'].encode(), p['start'], p['bytes'],
                              KIND[p['kind']], 1 if p['ro'] else 0, u)
    struct.pack_into('<I', out, 508, zlib.crc32(out[:508]))
    return bytes(out)


def parse_header(raw):
    refuse(len(raw) >= 512, 'header too short')
    raw = raw[:512]
    refuse(raw[:16] == HEADER_MAGIC, 'no NZEUS header magic')
    refuse(struct.unpack_from('<I', raw, 508)[0] == zlib.crc32(raw[:508]), 'header CRC mismatch')
    (_magic, version, size, card, fat_end, rs, re_, cid, fsha, count, _) = HEADER_HEAD.unpack_from(raw, 0)
    refuse(version == HEADER_VERSION and size == 512 and count <= 6, 'unknown header version')
    kinds = {v: k for k, v in KIND.items()}
    parts = []
    for i in range(count):
        name, start, nbytes, kind, flags, u = HEADER_PART.unpack_from(raw, 112 + 64 * i)
        parts.append({'num': i + 1, 'name': name.rstrip(b'\0').decode(), 'kind': kinds[kind], 'start': start,
                      'bytes': nbytes, 'ro': bool(flags & 1),
                      'uuid': str(uuid.UUID(bytes=u)) if any(u) else None})
    return {'card_bytes': card, 'fat_end': fat_end, 'region_start': rs, 'region_end': re_, 'cid': cid.hex(),
            'first_mib_sha256': fsha.hex(), 'parts': parts}


# ---------------------------------------------------------------------- survey
def same_area_as_card_io(layout):
    """The computed layout must be exactly the area card_io.py/region_io.py guard."""
    refuse(layout['card_bytes'] == region_io.CARD_BYTES and layout['region_start'] == region_io.REGION_START
           and layout['region_end'] == region_io.REGION_END
           and part(layout, 'nzhdr2')['start'] == region_io.HEADER2,
           'this layout is not the Linux area card_io.py writes (different card size?)')


def open_card_image(path, mode='rb'):
    """A full fake card IMAGE FILE of exactly the card size (never a device),
    opened unbuffered like card_io.py opens the real card."""
    p = image_path(path)
    refuse(p.stat().st_size == CARD_BYTES, f'{p} is not a {CARD_BYTES}-byte card image')
    return open(p, mode, buffering=0)


def survey(source, out_dir, log=print):
    """Read-only: card_io.py's survey code on a full card image file."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    with open_card_image(source) as card:
        return region_io.survey(card, out_dir, log)


# ----------------------------------------------------------------------- build
def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdin=subprocess.DEVNULL, **kw)


def e2fsck_clean(path, offset):
    r = subprocess.run(['e2fsck', '-fn', f'{path}?offset={offset}'], capture_output=True, text=True, check=False)
    refuse(r.returncode == 0, f'e2fsck found problems at offset {offset}:\n{r.stdout}{r.stderr}')


def e2fsck_fix(path):
    """Preen a temporary image file (never a card): 0 = clean, 1 = fixed."""
    r = subprocess.run(['e2fsck', '-fp', str(path)], capture_output=True, text=True, check=False)
    refuse(r.returncode in (0, 1), f'e2fsck could not clean {path}:\n{r.stdout}{r.stderr}')


def fs_info(path, offset):
    out = subprocess.run(['dumpe2fs', '-h', f'{path}?offset={offset}'], capture_output=True,
                         text=True, check=True).stdout
    keep = ('Filesystem volume name', 'Filesystem UUID', 'Filesystem features', 'Block count',
            'Free blocks', 'Block size', 'Inode count', 'Reserved block count', 'Journal size')
    return {k.strip(): v.strip() for k, v in (line.split(':', 1) for line in out.splitlines() if ':' in line)
            if k.strip() in keep}


def build(layout, out_dir, cid, first_mib_sha256, rootfs_dir=None, rootfs_ext4=None):
    refuse(bool(rootfs_dir) != bool(rootfs_ext4), 'give exactly one of --rootfs-dir / --rootfs-ext4')
    out_dir = Path(out_dir)
    # The candidate is a 44 GiB sparse file: only a Linux file system keeps it
    # small. On /mnt/c or /mnt/d (Windows drives) it would really take 44 GiB.
    refuse(not str(out_dir.resolve()).startswith('/mnt/'),
           f'{out_dir}: build on the WSL disk (e.g. ~/nzeus-region/...), not on a Windows drive under /mnt')
    out_dir.mkdir(parents=True, exist_ok=True)
    source = None
    cand = image_path(out_dir / 'region-candidate.img', must_exist=False)
    refuse(not cand.exists(), f'{cand} already exists - use a new folder')
    rs, re_ = layout['region_start'], layout['region_end']
    with open(cand, 'xb') as f:
        f.truncate(re_ - rs)
    uuids = {n: fs_uuid(n, cid) for n in ('nzroot', 'nzdata')}
    root, data = part(layout, 'nzroot'), part(layout, 'nzdata')
    rel_root, rel_data = root['start'] - rs, data['start'] - rs
    common = ['-t', 'ext4', '-b', '4096', '-F', '-q', '-E']
    lazy = 'lazy_itable_init=0,lazy_journal_init=0,nodiscard'
    if rootfs_dir:
        run(['mke2fs', *common, f'offset={rel_root},{lazy},root_owner=0:0', '-L', 'nzroot',
             '-U', uuids['nzroot'], '-d', str(rootfs_dir), str(cand), f'{root["bytes"] // KiB}k'])
    else:
        src = image_path(rootfs_ext4)
        source = {'file': src.name, 'bytes': src.stat().st_size, 'sha256': region_io.file_sha(src)}
        tmp = image_path(out_dir / 'nzroot-resized.tmp', must_exist=False)
        refuse(not tmp.exists(), f'{tmp} exists')
        with open(tmp, 'xb') as f:
            f.truncate(src.stat().st_size)
        sparse_copy(src, tmp)
        e2fsck_fix(tmp)
        # Grow (or shrink, if the root image was made bigger) to exactly nzroot.
        r = subprocess.run(['resize2fs', '-f', str(tmp), f'{root["bytes"] // KiB}K'], capture_output=True,
                           text=True, check=False, stdin=subprocess.DEVNULL)
        refuse(r.returncode == 0, f'resize2fs could not fit the root file system into {root["bytes"]} bytes:\n'
                                  f'{r.stdout}{r.stderr}')
        refuse(tmp.stat().st_size == root['bytes'], 'resized root has the wrong size')
        run(['tune2fs', '-L', 'nzroot', '-U', uuids['nzroot'], str(tmp)], capture_output=True)
        e2fsck_fix(tmp)
        sparse_copy(tmp, cand, rel_root)
        tmp.unlink()
    run(['mke2fs', *common, f'offset={rel_data},{lazy}', '-L', 'nzdata', '-U', uuids['nzdata'],
         '-m', '1', '-i', '65536', str(cand), f'{data["bytes"] // KiB}k'])
    header = make_header(layout, cid, first_mib_sha256, uuids)
    with open(cand, 'r+b') as f:
        for name in ('nzhdr', 'nzhdr2'):
            f.seek(part(layout, name)['start'] - rs)
            f.write(header)
    # The candidate must still be exactly the area (nothing written past its end)
    # and each header block must hold only the header (no file system spilled in).
    refuse(cand.stat().st_size == re_ - rs, 'candidate grew past the end of the Linux area')
    with open(cand, 'rb') as f:
        for name in ('nzhdr', 'nzhdr2'):
            f.seek(part(layout, name)['start'] - rs)
            blob = f.read(HEADER_BYTES)
            refuse(blob[:512] == header and not any(blob[512:]), f'{name} block is not exactly the header')
    for p in (root, data):
        e2fsck_clean(cand, p['start'] - rs)
    info = {'layout': layout, 'cid': cid, 'uuids': uuids, 'header_sha256': sha(header), 'rootfs_source': source,
            'filesystems': {p['name']: fs_info(cand, p['start'] - rs) for p in (root, data)}}
    (out_dir / 'build.json').write_text(json.dumps(info, indent=2) + '\n')
    (out_dir / 'nzroot-initramfs.conf').write_text(initramfs_conf(layout, cid, uuids))
    return info


def initramfs_conf(layout, cid, uuids):
    """Values the future initramfs check (PLAN.md 'Boot flow') compares with sysfs."""
    lines = ['# Generated by nz_region.py build - do not edit by hand.',
             f'NZ_CID={cid}', f'NZ_BLKDEVPARTS="{layout["blkdevparts"]}"']
    for p in layout['parts']:
        lines.append(f'NZ_P{p["num"]}_NAME={p["name"]} NZ_P{p["num"]}_START_SECTORS={p["start"] // 512} '
                     f'NZ_P{p["num"]}_SIZE_SECTORS={p["bytes"] // 512}')
    lines += [f'NZ_ROOT_UUID={uuids["nzroot"]}', f'NZ_DATA_UUID={uuids["nzdata"]}']
    return '\n'.join(lines) + '\n'


# ------------------------------------------------------------------------ plan
def plan(snapshot, snapshot_json, survey_dir, build_dir, out_dir, max_bytes=DEFAULT_MAX_PLAN,
         card_bytes=CARD_BYTES):
    """delta.json + region-after.blocks in out_dir (the card folder that also
    holds the survey; card_io.py region-install reads all three from there)."""
    out_dir = Path(out_dir)
    if max_bytes == DEFAULT_MAX_PLAN:
        max_bytes = region_io.plan_limit(survey_dir)   # + saved old data, with my KEEP_ALL note only
    for name in (region_io.DELTA, region_io.AFTER_DATA):
        refuse(not (out_dir / name).exists(), f'{out_dir / name} already exists - plans are never overwritten')
    snap_meta = json.loads(Path(snapshot_json).read_text())
    refuse(Path(snapshot).stat().st_size == snap_meta['bytes'] == SNAPSHOT_BYTES, 'snapshot size is not the usual extent')
    refuse(snap_meta.get('disk', {}).get('Size', card_bytes) == card_bytes, 'snapshot was taken from a different-size disk')
    refuse(hash_file_prefix(snapshot, snap_meta['bytes']) == snap_meta['sha256'],
           'snapshot image does not match snapshot.json (changed or incomplete)')
    prefix = parse_prefix(snapshot, card_bytes)
    refuse(prefix['state'] == 'closed', 'sector 0 is in the "open" (unscrambled) state; close the card first')
    layout = compute_layout(card_bytes, prefix['fat_end'])
    same_area_as_card_io(layout)
    built = json.loads((Path(build_dir) / 'build.json').read_text())
    refuse(built['layout'] == layout, 'the candidate was built for a different layout')
    surv, surv_sha, index = region_io.load_survey(survey_dir)
    refuse(surv['first_mib_sha256'] == prefix['first_mib_sha256']
           and surv['fat_boot_sector_sha256'] == prefix['fat_boot_sector_sha256'],
           'the survey and the snapshot were not taken from the same card state')
    rs, re_ = layout['region_start'], layout['region_end']
    cand = image_path(Path(build_dir) / 'region-candidate.img')
    refuse(cand.stat().st_size == re_ - rs, 'candidate image size is not the Linux area size')
    partial = out_dir / (region_io.AFTER_DATA + '.partial')
    blocks = []
    with open(cand, 'rb') as c, open(Path(survey_dir) / region_io.SURVEY_DATA, 'rb') as sdata, \
            open(partial, 'wb') as adata:
        offsets = {rs + x for x in blocks_with_data(c, 0, re_ - rs)} | set(index)
        for off in sorted(offsets):
            c.seek(off - rs)
            new = c.read(BLOCK)
            old = region_io.surveyed(sdata, index, off)
            if new != old:
                blocks.append({'offset': off, 'bytes': BLOCK, 'before': sha(old), 'after': sha(new)})
                adata.write(new)
                refuse(len(blocks) * BLOCK <= max_bytes,
                       f'plan writes more than the limit {max_bytes} bytes (stopped early)')
    result = {'kind': 'nzeus-linux-area', 'first_mib_sha256': prefix['first_mib_sha256'],
              'fat_boot_sector_sha256': prefix['fat_boot_sector_sha256'], 'fat_end': prefix['fat_end'],
              'snapshot_sha256': snap_meta['sha256'], 'card_bytes': card_bytes, 'region_start': rs,
              'region_end': re_, 'blkdevparts': layout['blkdevparts'], 'header_sha256': built['header_sha256'],
              'rootfs_source': built.get('rootfs_source'), 'candidate': str(cand.resolve()), 'candidate_base': rs,
              'survey_json_sha256': surv_sha, 'survey_area_sha256': surv['area_sha256'],
              'restore_source': region_io.SURVEY_DATA, 'after_data': region_io.AFTER_DATA,
              'after_data_sha256': region_io.file_sha(partial),
              'note': 'install only with card_io.py region-install (preflight + readback); this tool never writes a card',
              'layout': layout, 'blocks': blocks}
    check_plan(result, max_bytes)
    region_io.check_plan(result, max_bytes)
    os.replace(partial, out_dir / region_io.AFTER_DATA)
    (out_dir / region_io.DELTA).write_text(json.dumps(result, indent=2) + '\n')
    return result


def check_plan(p, max_bytes=DEFAULT_MAX_PLAN):
    """Every block strictly inside the Linux area, aligned, sorted, non-overlapping."""
    layout = compute_layout(p['card_bytes'], p['fat_end'])
    refuse(p['layout'] == layout and p['blkdevparts'] == layout['blkdevparts'], 'plan layout is not the computed layout')
    rs, re_ = layout['region_start'], layout['region_end']
    floor = max(p['fat_end'] + GUARD, SNAPSHOT_BYTES, rs)
    last_end = 0
    for b in p['blocks']:
        off, n = b['offset'], b['bytes']
        refuse(off % 512 == 0 and n % 512 == 0 and n > 0, f'unaligned block {off}')
        refuse(floor <= off and off + n <= re_, f'block {off}+{n} is OUTSIDE the Linux area [{rs}, {re_})')
        refuse(off >= last_end, f'blocks overlap or are not sorted at {off}')
        last_end = off + n
    total = sum(b['bytes'] for b in p['blocks'])
    refuse(total <= max_bytes, f'plan writes {total} bytes, more than the limit {max_bytes}')
    return total


# -------------------------------------------------------------------- selftest
def make_fake_card(path, card_bytes, fat_sectors, extra_partition=False):
    """Sparse fake stock card: scrambled sector 0, FAT32 at 1 MiB, boot files."""
    with open(path, 'xb') as f:
        f.truncate(card_bytes)
    # -g 64/32: a geometry that divides the size, so mkfs.fat does not round it down.
    run(['mkfs.fat', '-F', '32', '-s', '64', '-h', '2048', '-g', '64/32', '-n', 'NO NAME',
         '--offset=2048', str(path), str(fat_sectors // 2)], capture_output=True)
    made = struct.unpack_from('<I', read_at(path, FAT_START, 512), 0x20)[0]
    refuse(made == fat_sectors, f'mkfs.fat made {made} sectors, wanted {fat_sectors}')
    mbr = bytearray(512)
    struct.pack_into('<B3xBxxxII', mbr, 446, 0, 0x0C, 2048, fat_sectors)
    if extra_partition:
        struct.pack_into('<B3xBxxxII', mbr, 462, 0, 0x83, 2048 + fat_sectors, 2048)
    mbr[510:512] = b'\x55\xaa'
    with open(path, 'r+b') as f:
        f.write(unscramble(bytes(mbr)))
    img = f'{path}@@{FAT_START}'
    run(['mmd', '-i', img, '::/cubegm'], capture_output=True)
    for name, size in (('advapi32.dll', 300000), ('Bubbles.scr', 4096), ('dtb.bin', 2048)):
        src = Path(path).parent / name
        src.write_bytes(hashlib.sha256(name.encode()).digest() * (size // 32))
        run(['mcopy', '-i', img, str(src), f'::/cubegm/{name}'], capture_output=True)


def take_fake_snapshot(card, snap_dir):
    snap_dir.mkdir(parents=True, exist_ok=True)
    before = snap_dir / 'before.img'
    with open(before, 'xb') as f:
        f.truncate(SNAPSHOT_BYTES)
    sparse_copy(card, before, 0, SNAPSHOT_BYTES)
    meta = {'bytes': SNAPSHOT_BYTES, 'sha256': hash_file_prefix(before, SNAPSHOT_BYTES),
            'disk': {'Number': 5, 'Size': CARD_BYTES, 'BusType': 'USB', 'IsSystem': False, 'IsBoot': False}}
    (snap_dir / 'snapshot.json').write_text(json.dumps(meta, indent=2))
    return before, snap_dir / 'snapshot.json'


def copy_card(src, dst):
    """Sparse copy of a fake card (cheap: only the FAT metadata is real data)."""
    with open(dst, 'xb') as f:
        f.truncate(CARD_BYTES)
    sparse_copy(src, dst, 0, CARD_BYTES)
    return dst


def area_diff_fast(path, folder):
    """Selftest helper for SPARSE fake cards: [(offset, sha256)] of the area
    blocks that differ from the survey in `folder`. Same answer as
    region_io.compare_area (which reads all 44 GiB), but only reads data."""
    _, _, index = region_io.load_survey(folder)
    diff = []
    with open(image_path(path), 'rb') as f, open(Path(folder) / region_io.SURVEY_DATA, 'rb') as sdata:
        offsets = set(blocks_with_data(f, region_io.REGION_START, region_io.REGION_END)) | set(index)
        for off in sorted(offsets):
            f.seek(off)
            blk = f.read(BLOCK)
            if blk != region_io.surveyed(sdata, index, off):
                diff.append((off, sha(blk)))
    return diff


def selftest(workdir, rootfs_ext4=None):
    work = Path(tempfile.mkdtemp(prefix='nz-region-selftest-', dir=workdir))
    results = []

    def check(name, cond, detail=''):
        results.append((name, bool(cond)))
        print(('PASS ' if cond else 'FAIL ') + name + (f' ({detail})' if detail and not cond else ''), flush=True)

    def expect_refusal(name, fn, needle):
        try:
            fn()
        except Refused as e:
            check(name, needle in str(e), str(e))
            return
        check(name, False, 'was NOT refused')

    def quiet(*_):
        pass

    def on(path, fn, folder, mode='rb', log=quiet):
        """card_io.py's region code on a fake card: the selftest's stand-in for
        card_io's disk checks is 'a card-sized regular file in this folder'."""
        refuse(Path(path).resolve().is_relative_to(work.resolve()), 'fake card outside the selftest folder')
        with open_card_image(path, mode) as card:
            return fn(card, folder, log)

    fat_sectors = SNAPSHOT_BYTES // 512 - 2048        # FAT32 fills the established extent
    card = work / 'fake-card.img'
    make_fake_card(card, CARD_BYTES, fat_sectors)
    folder = work / 'card-region'                      # plays linux-port/rootfs/card-region (D: junction)
    snap, snap_json = take_fake_snapshot(card, folder)
    snap_sha = json.loads(snap_json.read_text())['sha256']
    prefix = parse_prefix(snap)
    check('inspect: closed (scrambled) table found', prefix['state'] == 'closed')
    check('inspect: FAT end = 16357785600', prefix['fat_end'] == SNAPSHOT_BYTES, prefix['fat_end'])
    layout = compute_layout(CARD_BYTES, prefix['fat_end'])
    expected = ('blkdevparts=mmcblk0:15599M@1M(nzstock)ro,4M@16G(nzhdr)ro,4G@16388M(nzroot),'
                '40416M@20484M(nzdata),4M@60900M(nzhdr2)ro')
    check('layout: blkdevparts string', layout['blkdevparts'] == expected, layout['blkdevparts'])
    check('layout: region ends 4 MiB-aligned before card end',
          layout['region_end'] == 63862472704 and layout['region_end'] <= CARD_BYTES)
    same_area_as_card_io(layout)
    check('layout: exactly the area card_io.py guards (region_io.py constants)', True)

    # ---- survey with card_io.py's own code
    surv = on(card, region_io.survey, folder)
    check('survey (card_io code): fake area is all zero, whole-area SHA-256 recorded',
          surv['zero_blocks'] == surv['blocks'] == 712320 and not surv['nonzero_blocks']
          and len(surv['area_sha256']) == 64)
    expect_refusal('refuse: a second survey into the same folder (surveys must be fresh)',
                   lambda: on(card, region_io.survey, folder), 'fresh')

    # ---- build + plan
    if rootfs_ext4:
        build(layout, work / 'build', TEST_CARD_CID, prefix['first_mib_sha256'], rootfs_ext4=rootfs_ext4)
    else:
        rootdir = work / 'rootfs'
        for d in ('sbin', 'etc', 'data', 'dev', 'proc', 'sys', 'tmp', 'run'):
            (rootdir / d).mkdir(parents=True, exist_ok=True)
        (rootdir / 'sbin/init').write_text('#!/bin/sh\necho fake init\n')
        (rootdir / 'sbin/init').chmod(0o755)
        (rootdir / 'etc/nzeus-rootfs-release').write_text('NZEUS_ROOTFS=1\nNZEUS_ROOTFS_LAYOUT=1\n')
        build(layout, work / 'build', TEST_CARD_CID, prefix['first_mib_sha256'], rootfs_dir=rootdir)
    check('build: both ext4 file systems pass e2fsck -fn', True)
    hdr = parse_header(read_at(work / 'build/region-candidate.img', 0, 512))
    check('build: header parses, CRC ok, CID and FAT end recorded',
          hdr['cid'] == TEST_CARD_CID and hdr['fat_end'] == SNAPSHOT_BYTES and len(hdr['parts']) == 5)
    expect_refusal('refuse: build folder on a Windows drive (/mnt/...)',
                   lambda: build(layout, '/mnt/d/nz-region-x', TEST_CARD_CID, prefix['first_mib_sha256'],
                                 rootfs_dir=work), 'WSL disk')

    p = plan(snap, snap_json, folder, work / 'build', folder)
    total = sum(b['bytes'] for b in p['blocks'])
    check('plan: blocks only inside the Linux area', all(
        p['region_start'] <= b['offset'] and b['offset'] + b['bytes'] <= p['region_end'] for b in p['blocks']))
    check('plan: size is small', 0 < total <= DEFAULT_MAX_PLAN, total)
    check('plan: region-after.blocks holds exactly the plan blocks',
          (folder / region_io.AFTER_DATA).stat().st_size == total)
    check('plan folder passes the Windows-side check (region_io.py check)',
          region_io.check_folder(folder)['blocks'] == p['blocks'])
    print(f'     plan: {len(p["blocks"])} blocks, {total / MiB:.1f} MiB', flush=True)
    (work / 'plan-again').mkdir()
    p2 = plan(snap, snap_json, folder, work / 'build', work / 'plan-again')
    check('plan is reproducible', p2['blocks'] == p['blocks'] and p2['after_data_sha256'] == p['after_data_sha256'])
    installed_state = [(b['offset'], b['after']) for b in p['blocks']]

    # ---- install with card_io.py's own code on a COPY, then prove nothing else changed
    inst = copy_card(card, work / 'card-installed.img')
    n = on(inst, region_io.install, folder, 'r+b')
    check('install (card_io code): every block preflighted, written and read back', n == len(p['blocks']))
    check('after install: first 16357785600 bytes (MBR + FAT32) unchanged',
          hash_file_prefix(inst, SNAPSHOT_BYTES) == snap_sha)
    with open(inst, 'rb') as f:
        outside = [r for r in data_ranges(f, SNAPSHOT_BYTES, CARD_BYTES)
                   if not (p['region_start'] <= r[0] and r[1] <= p['region_end'])]
    check('after install: no data between FAT end and area start, none after area end', not outside, outside[:3])
    check('after install: area = survey + exactly the plan blocks', area_diff_fast(inst, folder) == installed_state)
    for name in ('nzroot', 'nzdata'):
        e2fsck_clean(inst, part(layout, name)['start'])
    check('after install: ext4 nzroot + nzdata clean on the installed copy', True)
    h1 = parse_header(read_at(inst, part(layout, 'nzhdr')['start'], 512))
    h2 = parse_header(read_at(inst, part(layout, 'nzhdr2')['start'], 512))
    check('after install: main and backup header identical and valid', h1 == h2)
    got = subprocess.run(['mtype', '-i', f'{inst}@@{FAT_START}', '::/cubegm/advapi32.dll'],
                         capture_output=True, check=True).stdout
    check('after install: stock boot file still readable and identical', got == (work / 'advapi32.dll').read_bytes())
    seen = []
    state, _ = on(inst, region_io.verify, folder, log=lambda *a: seen.append(' '.join(map(str, a))))
    check('verify (card_io code, reads all 44 GiB): "installed", stock part = snapshot',
          state == 'installed' and any('SAME_AS_SNAPSHOT' in line for line in seen), (state, seen[-2:]))

    # ---- restore = only the touched blocks, back to the survey
    r = on(inst, region_io.restore, folder, 'r+b')
    check('restore (card_io code): exactly the touched blocks written back', r == len(p['blocks']), r)
    check('after restore: first 16357785600 bytes unchanged', hash_file_prefix(inst, SNAPSHOT_BYTES) == snap_sha)
    check('after restore: area = survey', area_diff_fast(inst, folder) == [])
    (work / 'resurvey').mkdir()
    s2 = on(inst, region_io.survey, work / 'resurvey')
    check('after restore: a fresh survey gives the same whole-area SHA-256',
          s2['area_sha256'] == surv['area_sha256'] and s2['nonzero_blocks'] == surv['nonzero_blocks'])
    check('restore again: nothing left to do', on(inst, region_io.restore, folder, 'r+b') == 0)

    # ---- interrupted install (first half written): restore still works
    half = copy_card(card, work / 'card-interrupted.img')
    k = len(p['blocks']) // 2
    with open(half, 'r+b') as f, open(folder / region_io.AFTER_DATA, 'rb') as a:
        for i, b in enumerate(p['blocks'][:k]):
            a.seek(i * BLOCK)
            f.seek(b['offset'])
            f.write(a.read(BLOCK))
    r = on(half, region_io.restore, folder, 'r+b')
    check('interrupted install: restore writes back only the written half', r == k and area_diff_fast(half, folder) == [])

    # ---- Linux has used the area: restore refuses, reset returns the whole area to the survey
    used = copy_card(card, work / 'card-used.img')
    on(used, region_io.install, folder, 'r+b')
    in_root = next(b['offset'] for b in p['blocks'] if b['offset'] >= part(layout, 'nzroot')['start'])
    new_file = part(layout, 'nzdata')['start'] + 7 * GiB + 123 * BLOCK
    with open(used, 'r+b') as f:
        f.seek(in_root + 1024)
        f.write(b'\x01')                            # e.g. the root superblock after a mount
        f.seek(new_file)
        f.write(b'a file in /data ' * 8192)         # 128 KiB of user data, not a plan block
    dirty = area_diff_fast(used, folder)
    expect_refusal('refuse: restore after Linux wrote to the area', lambda: on(used, region_io.restore, folder, 'r+b'),
                   'neither')
    check('refused restore wrote nothing', area_diff_fast(used, folder) == dirty)
    k = on(used, region_io.reset, folder, 'r+b')
    check('reset (card_io code): wrote only the blocks that differ', k == len(dirty) == len(p['blocks']) + 2, k)
    check('after reset: area = survey, first 16357785600 bytes unchanged',
          area_diff_fast(used, folder) == [] and hash_file_prefix(used, SNAPSHOT_BYTES) == snap_sha)
    state, _ = on(used, region_io.verify, folder)
    check('verify (reads all 44 GiB) after reset: "surveyed"', state == 'surveyed', state)
    foreign = copy_card(card, work / 'card-foreign.img')
    with open(foreign, 'r+b') as f:
        f.seek(region_io.REGION_START)
        f.write(b'NOT-OURS' * 64)
    expect_refusal('refuse: reset of an area that does not carry our header',
                   lambda: on(foreign, region_io.reset, folder, 'r+b'), 'not our Linux area')
    check('refused reset wrote nothing', [o for o, _ in area_diff_fast(foreign, folder)] == [region_io.REGION_START])

    # ---- stale survey / changed card / smaller card: refused before any write
    stale = copy_card(card, work / 'card-stale.img')
    victim = p['blocks'][-1]['offset']
    with open(stale, 'r+b') as f:
        f.seek(victim)
        f.write(b'\xa5' * 512)
    expect_refusal('refuse: stale survey (area changed since the survey)',
                   lambda: on(stale, region_io.install, folder, 'r+b'), 'stale')
    check('stale case: nothing was written', [o for o, _ in area_diff_fast(stale, folder)] == [victim])
    changed = copy_card(card, work / 'card-changed.img')
    with open(changed, 'r+b') as f:
        f.seek(4096)
        f.write(b'x')                               # inside the first MiB
    expect_refusal('refuse: changed card (first MiB differs)',
                   lambda: on(changed, region_io.install, folder, 'r+b'), 'first MiB')
    with open(changed, 'r+b') as f:
        f.seek(4096)
        f.write(b'\0')
        f.seek(FAT_START + 0x47)
        f.write(b'Y')                               # volume label inside the FAT boot sector
    expect_refusal('refuse: changed card (FAT boot sector differs)',
                   lambda: on(changed, region_io.install, folder, 'r+b'), 'FAT boot sector')
    expect_refusal('refuse: restore on a changed card', lambda: on(changed, region_io.restore, folder, 'r+b'),
                   'FAT boot sector')
    check('changed-card cases: nothing was written', area_diff_fast(changed, folder) == [])
    small = work / 'card-small.img'
    with open(small, 'xb') as f:
        f.truncate(region_io.REGION_END - MiB)
    sparse_copy(card, small, 0, SNAPSHOT_BYTES)
    expect_refusal('refuse: card image of another size (stand-in for the Get-Disk size check)',
                   lambda: on(small, region_io.install, folder, 'r+b'), 'card image')
    with open(small, 'r+b', buffering=0) as c:
        expect_refusal('refuse: smaller card even without that check (area end unreadable)',
                       lambda: region_io.install(c, folder, quiet), 'smaller')

    # ---- damaged plan folder files
    tam = work / 'card-region-tampered'
    tam.mkdir()
    for name in (region_io.SURVEY_JSON, region_io.SURVEY_DATA, region_io.DELTA, region_io.AFTER_DATA):
        shutil.copy(folder / name, tam / name)
    tcard = copy_card(card, work / 'card-tamper.img')
    with open(tam / region_io.AFTER_DATA, 'r+b') as f:
        f.seek(100)
        f.write(b'!')
    expect_refusal('refuse: region-after.blocks changed after planning',
                   lambda: on(tcard, region_io.install, tam, 'r+b'), 'does not belong')
    shutil.copy(folder / region_io.AFTER_DATA, tam / region_io.AFTER_DATA)
    with open(tam / region_io.SURVEY_JSON, 'a') as f:
        f.write(' ')
    expect_refusal('refuse: survey file changed after planning',
                   lambda: on(tcard, region_io.install, tam, 'r+b'), 'not made from this survey')
    check('tampered cases: nothing was written', area_diff_fast(tcard, folder) == [])

    # ---- plan rules (region_io.check_plan = what card_io.py checks on Windows)
    def mutated(change):
        q = json.loads(json.dumps(p))
        change(q)
        return q
    blk = {'bytes': BLOCK, 'before': ZERO_SHA, 'after': 'f' * 64}
    for name, q, needle in (
            ('a block inside the stock FAT32 extent',
             mutated(lambda q: q['blocks'].insert(0, {**blk, 'offset': SNAPSHOT_BYTES - BLOCK})), 'OUTSIDE'),
            ('a block in the gap before the area',
             mutated(lambda q: q['blocks'].insert(0, {**blk, 'offset': REGION_START - BLOCK})), 'OUTSIDE'),
            ('a block after the area end',
             mutated(lambda q: q['blocks'].append({**blk, 'offset': p['region_end']})), 'OUTSIDE'),
            ('an unaligned block', mutated(lambda q: q['blocks'][-1].update(offset=q['blocks'][-1]['offset'] + 512)),
             'aligned'),
            ('overlapping blocks', mutated(lambda q: q['blocks'].append(dict(q['blocks'][-1]))), 'overlap'),
            ('a plan for another area', mutated(lambda q: q.update(region_end=q['region_end'] - 4 * MiB)),
             'different card'),
            ('a plan bigger than the 2 GiB limit',
             mutated(lambda q: q.update(blocks=[{**blk, 'offset': REGION_START + 4 * MiB + i * BLOCK}
                                                for i in range(DEFAULT_MAX_PLAN // BLOCK + 1)])), 'limit')):
        expect_refusal(f'refuse (card_io rules): {name}', lambda q=q: region_io.check_plan(q), needle)
    bad = mutated(lambda q: q.update(blkdevparts=q['blkdevparts'].replace('16388M', '16384M')))
    expect_refusal('refuse: edited partition line', lambda: check_plan(bad), 'layout')
    expect_refusal('refuse: oversized plan (--max-bytes)', lambda: check_plan(p, max_bytes=BLOCK), 'limit')
    (work / 'plan-small').mkdir()
    expect_refusal('refuse: plan stops early when over --max-bytes',
                   lambda: plan(snap, snap_json, folder, work / 'build', work / 'plan-small', max_bytes=BLOCK),
                   'stopped early')
    check('over-limit plan left no delta.json', not (work / 'plan-small' / region_io.DELTA).exists())
    expect_refusal('refuse: device paths', lambda: image_path('/dev/mmcblk0'), '/dev')
    expect_refusal('refuse: Windows raw disk paths', lambda: image_path(r'\\.\PhysicalDrive5'), 'Windows')

    # ---- junk in the area: surveyed, planned with the junk as preimage, put back by restore
    junk = work / 'card-region-junk'
    junk.mkdir()
    surv2 = on(stale, region_io.survey, junk)       # the stale card has 512 bytes of 0xA5 in the area
    check('survey: junk block found', [b['offset'] for b in surv2['nonzero_blocks']] == [victim])
    p3 = plan(snap, snap_json, junk, work / 'build', junk)
    jb = [b for b in p3['blocks'] if b['offset'] == victim]
    check('plan: junk block has the junk as preimage', jb and jb[0]['before'] == surv2['nonzero_blocks'][0]['sha256'])
    on(stale, region_io.install, junk, 'r+b')
    check('install over junk: area = plan',
          area_diff_fast(stale, junk) == [(b['offset'], b['after']) for b in p3['blocks']])
    on(stale, region_io.restore, junk, 'r+b')
    check('restore puts the surveyed junk back', area_diff_fast(stale, junk) == []
          and read_at(stale, victim, 512) == b'\xa5' * 512)
    saved = region_io.MAX_SURVEY_DATA
    region_io.MAX_SURVEY_DATA = BLOCK                # pretend the cap is one block
    try:
        (work / 'survey-capped').mkdir()
        on(stale, region_io.install, junk, 'r+b')
        expect_refusal('refuse: survey of an area that is in use (more non-zero data than the cap)',
                       lambda: on(stale, region_io.survey, work / 'survey-capped'), 'in use')
    finally:
        region_io.MAX_SURVEY_DATA = saved
    with open(junk / region_io.SURVEY_DATA, 'r+b') as f:
        f.write(b'\0')
    expect_refusal('refuse: damaged region-before.blocks', lambda: region_io.check_folder(junk), 'does not match')

    # ---- snapshot / card shape refusals (planning side)
    expect_refusal('refuse: FAT32 reaching into the guard gap',
                   lambda: compute_layout(CARD_BYTES, REGION_START - GUARD + 512), 'untouched gap')
    with open(snap, 'r+b') as f:
        f.seek(5 * MiB)
        f.write(b'x')
    (work / 'plan-x').mkdir()
    expect_refusal('refuse: snapshot changed after snapshot.json',
                   lambda: plan(snap, snap_json, folder, work / 'build', work / 'plan-x'), 'snapshot image')
    two = work / 'two-part' / 'card.img'
    two.parent.mkdir()
    make_fake_card(two, CARD_BYTES, fat_sectors, extra_partition=True)
    expect_refusal('refuse: card with a second partition', lambda: parse_prefix(two), 'more than one partition')
    expect_refusal('refuse: card of another size',
                   lambda: compute_layout(8 * GiB, SNAPSHOT_BYTES), 'too small')

    failed = [n for n, ok in results if not ok]
    print(f'\nSELFTEST {"PASSED" if not failed else "FAILED"}: {len(results) - len(failed)}/{len(results)}'
          f' (work folder {work})')
    return not failed


# ------------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('layout')
    a.add_argument('--card-bytes', type=int, default=CARD_BYTES)
    a.add_argument('--fat-end', type=int, default=SNAPSHOT_BYTES)
    a = sub.add_parser('inspect')
    a.add_argument('snapshot')
    a.add_argument('--card-bytes', type=int, default=CARD_BYTES)
    a = sub.add_parser('survey', help='survey a full card IMAGE FILE (the real card: card_io.py region-survey)')
    a.add_argument('--source', required=True, help='full card image file (exactly the card size)')
    a.add_argument('--out', required=True)
    a = sub.add_parser('build')
    a.add_argument('--snapshot', required=True)
    a.add_argument('--out', required=True, help='a folder on the WSL disk (not /mnt/...)')
    a.add_argument('--cid', default=TEST_CARD_CID)
    a.add_argument('--rootfs-dir')
    a.add_argument('--rootfs-ext4', help=f'root file system image (the Debian build: {DEFAULT_ROOTFS})')
    a.add_argument('--card-bytes', type=int, default=CARD_BYTES)
    a = sub.add_parser('plan')
    a.add_argument('--snapshot', required=True)
    a.add_argument('--snapshot-json', required=True)
    a.add_argument('--survey', required=True, help='folder with region-survey.json + region-before.blocks')
    a.add_argument('--build', required=True)
    a.add_argument('--out', help='folder for delta.json + region-after.blocks (default: the survey folder)')
    a.add_argument('--max-bytes', type=int, default=DEFAULT_MAX_PLAN)
    a = sub.add_parser('check')
    a.add_argument('folder', help='plan folder (or its delta.json)')
    a.add_argument('--max-bytes', type=int, default=DEFAULT_MAX_PLAN)
    a = sub.add_parser('selftest')
    a.add_argument('--workdir', default=tempfile.gettempdir())
    a.add_argument('--rootfs-ext4', help='use a real root file system image instead of a tiny test tree')
    args = ap.parse_args(argv)
    try:
        if args.cmd == 'layout':
            print(json.dumps(compute_layout(args.card_bytes, args.fat_end), indent=2))
        elif args.cmd == 'inspect':
            pre = parse_prefix(args.snapshot, args.card_bytes)
            print(json.dumps({'prefix': pre, 'layout': compute_layout(args.card_bytes, pre['fat_end'])}, indent=2))
        elif args.cmd == 'survey':
            survey(args.source, args.out)
        elif args.cmd == 'build':
            pre = parse_prefix(args.snapshot, args.card_bytes)
            info = build(compute_layout(args.card_bytes, pre['fat_end']), args.out, args.cid,
                         pre['first_mib_sha256'], args.rootfs_dir,
                         args.rootfs_ext4 and os.path.expanduser(args.rootfs_ext4))
            print('BUILD_COMPLETE header', info['header_sha256'])
        elif args.cmd == 'plan':
            p = plan(args.snapshot, args.snapshot_json, args.survey, args.build, args.out or args.survey,
                     args.max_bytes)
            print('PLAN_COMPLETE', len(p['blocks']), 'blocks;', sum(b['bytes'] for b in p['blocks']), 'bytes',
                  '(nothing written to any card)')
        elif args.cmd == 'check':
            folder = Path(args.folder)
            folder = folder if folder.is_dir() else folder.parent
            limit = args.max_bytes if args.max_bytes != DEFAULT_MAX_PLAN else region_io.plan_limit(folder)
            total = check_plan(json.loads((folder / region_io.DELTA).read_text()), limit)
            region_io.check_folder(folder)
            print('PLAN_OK', total, 'bytes, all inside the Linux area; data files belong to the plan')
        elif args.cmd == 'selftest':
            return 0 if selftest(args.workdir, args.rootfs_ext4) else 1
    except Refused as e:
        print('REFUSED:', e, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
