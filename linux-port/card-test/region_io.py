r"""Linux-area ("region") card logic for linux-port/rootfs/PLAN.md.

Shared by card_io.py (the real card, elevated, Windows) and the nz_region.py
selftest (a FAKE 64 GB card file in WSL). This module never opens a disk: every
function gets an already-open binary file object `card`. The only code that
opens \\.\PhysicalDrive5 stays card_io.py, after its usual disk checks.

The area is [16 GiB, 60,904 MiB) of the 63,864,569,856-byte test card. Nothing
here ever writes below it (the first 16,357,785,600 bytes = MBR + stock FAT32,
what card_io.py snapshot copies) or after it (the last 2 MiB of the card).

Files in the profile folder (linux-port/rootfs/card-region -> D: junction):
  region-survey.json     the area as it was: offset + SHA-256 of every 64 KiB
                         block that is not all zero, SHA-256 of the whole area,
                         card identity hashes
  region-before.blocks   those non-zero blocks back to back, in survey order
                         (zero blocks are not stored: they are zero). A plain
                         file, not a sparse image: NTFS files are not sparse by
                         default and WSL cannot see holes through /mnt/d.
  delta.json             the sector plan from nz_region.py plan
  region-after.blocks    the new content of every plan block, in plan order

Actions (card_io.py region-*): survey (read-only), install (preflight every
preimage, then write + read back every block), restore (only the blocks the
install touched, back to the surveyed content), reset (return the WHOLE area to
the surveyed content after Linux has written to it; writes only blocks that
differ), verify (read-only: does the area match the survey or the install?).

Run directly (no disk access, files only):
  python region_io.py check <folder>     re-check delta.json + its data files
"""
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path

KiB, MiB, GiB = 1 << 10, 1 << 20, 1 << 30
CARD_BYTES = 63864569856          # card_io.py CARD_SIZE
STOCK_EXTENT = 16357785600        # card_io.py IMAGE_SIZE: never written by region actions
FAT_BOOT = 1 * MiB                # stock FAT32 boot sector (LBA 2048)
REGION_START = 16 * GiB           # first byte of the Linux area
REGION_END = 60904 * MiB          # last 4 MiB boundary of the card (card end - 2 MiB)
HEADER2 = REGION_END - 4 * MiB    # backup header partition (nzhdr2)
BLOCK = 64 * KiB                  # plan granularity, as prepare_card.py
CHUNK = 4 * MiB                   # sequential read size (as card_io.py)
# Biggest plan we accept. A Debian minbase root with python3 uses ~0.3-0.7 GiB;
# the plan is that data + ext4 metadata (+ a few MiB for the empty nzdata). 2 GiB
# leaves room for a bigger root but still refuses runaway plans (a candidate that
# lost its holes, or an area that reads 0xFF, would be ~44 GiB).
MAX_PLAN_BYTES = 2 * GiB
# More non-zero data than this in the area means someone uses it: stop and ask.
MAX_SURVEY_DATA = 1 * GiB
# My decision for ONE run: if this file is in the run's folder, the survey keeps
# EVERY non-zero block of the area (up to the whole ~43.5 GiB) instead of stopping,
# so old data there is saved to the PC before the install may overwrite it.
# 2026-10-10: the GREEN console's card holds old GoPro HERO10 video data there.
KEEP_ALL = 'owner-keep-all-area-data.txt'
ZERO = bytes(BLOCK)
ZERO_SHA = hashlib.sha256(ZERO).hexdigest()
ZERO_CHUNK = bytes(CHUNK)
SURVEY_JSON = 'region-survey.json'
SURVEY_DATA = 'region-before.blocks'
DELTA = 'delta.json'
AFTER_DATA = 'region-after.blocks'
HEX64 = re.compile('[0-9a-f]{64}')


class Refused(Exception):
    """A safety rule said no. Raised BEFORE the first write unless it says otherwise."""


def need(cond, why):
    if not cond:
        raise Refused(why)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for data in iter(lambda: f.read(CHUNK), b''):
            h.update(data)
    return h.hexdigest()


def read_exact(card, offset, count):
    try:
        card.seek(offset)
        data = card.read(count)
    except OSError as e:
        raise Refused(f'cannot read {count} bytes at {offset}: {e}') from e
    need(data is not None and len(data) == count, f'short read at {offset} (card smaller than expected?)')
    return data


def card_identity(card):
    """Hashes of the first MiB (scrambled MBR) and of the FAT32 boot sector.
    Also proves the card reaches the end of the area (a smaller card fails)."""
    first = read_exact(card, 0, MiB)
    boot = read_exact(card, FAT_BOOT, 512)
    read_exact(card, REGION_END - BLOCK, BLOCK)
    return {'first_mib_sha256': sha(first), 'fat_boot_sector_sha256': sha(boot)}


def hash_stock(card, log):
    """SHA-256 of the first 16,357,785,600 bytes (what card_io.py snapshot hashes)."""
    h = hashlib.sha256()
    for pos in range(0, STOCK_EXTENT, CHUNK):
        h.update(read_exact(card, pos, min(CHUNK, STOCK_EXTENT - pos)))
        if pos % (4 * GiB) == 0:
            log(f'stock {pos / STOCK_EXTENT:.0%}')
    return h.hexdigest()


def write_verified(card, offset, data, check):
    """Write one run inside the area, flush it to the card, read it back."""
    need(REGION_START <= offset and offset + len(data) <= REGION_END and offset >= STOCK_EXTENT,
         f'write {offset}+{len(data)} is OUTSIDE the Linux area')
    card.seek(offset)
    need(card.write(data) == len(data), f'short write at {offset}')
    os.fsync(card.fileno())
    need(check(read_exact(card, offset, len(data))), f'Readback failed at {offset} (the card now holds '
         f'unknown data there: run region-restore or region-reset, then tell Claude)')


# ------------------------------------------------------------------ plan rules
def plan_limit(folder):
    """MAX_PLAN_BYTES, plus - only with my KEEP_ALL note in the folder -
    the size of the old data the survey saved there: the install then also writes
    zeros over that old data, and may do exactly that much more."""
    folder = Path(folder)
    limit = MAX_PLAN_BYTES
    if (folder / KEEP_ALL).exists() and (folder / SURVEY_DATA).exists():
        limit += (folder / SURVEY_DATA).stat().st_size
    return limit


def check_plan(plan, limit=MAX_PLAN_BYTES):
    """Every block one aligned 64 KiB block strictly inside the area, sorted,
    no overlap, real before/after hashes, total within the limit (plan_limit)."""
    need(plan.get('kind') == 'nzeus-linux-area', 'not a Linux-area plan (delta.json from nz_region.py plan)')
    need(plan.get('card_bytes') == CARD_BYTES and plan.get('region_start') == REGION_START
         and plan.get('region_end') == REGION_END, 'plan is for a different card or area')
    parts = {p['name']: p for p in plan['layout']['parts']}
    need(parts['nzhdr']['start'] == REGION_START and parts['nzhdr2']['start'] == HEADER2,
         'plan headers are not where this tool expects them')
    need(HEX64.fullmatch(str(plan.get('header_sha256', ''))), 'plan has no header hash')
    blocks = plan.get('blocks')
    need(isinstance(blocks, list) and blocks, 'plan has no blocks')
    last_end = 0
    for b in blocks:
        off, n = b.get('offset'), b.get('bytes')
        need(type(off) is int and type(n) is int, f'bad block entry {b}')
        need(n == BLOCK and off % BLOCK == 0, f'block {off}+{n} is not one aligned 64 KiB block')
        need(off >= STOCK_EXTENT and REGION_START <= off and off + n <= REGION_END,
             f'block {off}+{n} is OUTSIDE the Linux area [{REGION_START}, {REGION_END})')
        need(off >= last_end, f'blocks overlap or are not sorted at {off}')
        need(HEX64.fullmatch(str(b.get('before'))) and HEX64.fullmatch(str(b.get('after')))
             and b['before'] != b['after'], f'block {off}: bad before/after hashes')
        last_end = off + n
    total = len(blocks) * BLOCK
    need(total <= limit, f'plan writes {total} bytes, more than the limit {limit}')
    return total


def load_plan(folder):
    plan = json.loads((Path(folder) / DELTA).read_text())
    check_plan(plan, plan_limit(folder))
    return plan


# ---------------------------------------------------------------------- survey
def load_survey(folder):
    """(survey dict, SHA-256 of region-survey.json, {offset: (index, sha256)})."""
    folder = Path(folder)
    raw = (folder / SURVEY_JSON).read_bytes()
    s = json.loads(raw)
    need(s.get('kind') == 'nzeus-linux-area-survey' and s.get('card_bytes') == CARD_BYTES
         and s.get('region_start') == REGION_START and s.get('region_end') == REGION_END
         and s.get('block') == BLOCK, 'survey is for a different card or area')
    offsets = [b['offset'] for b in s['nonzero_blocks']]
    need(offsets == sorted(set(offsets)) and all(
        type(o) is int and o % BLOCK == 0 and REGION_START <= o and o + BLOCK <= REGION_END for o in offsets),
        'survey block list is damaged')
    data = folder / SURVEY_DATA
    need(data.stat().st_size == len(offsets) * BLOCK and file_sha(data) == s['data_sha256'],
         f'{SURVEY_DATA} does not match {SURVEY_JSON} (changed or incomplete)')
    index = {b['offset']: (i, b['sha256']) for i, b in enumerate(s['nonzero_blocks'])}
    return s, sha(raw), index


def surveyed(sdata, index, offset):
    """What the survey saw at `offset`: zeros, or the stored block (hash-checked)."""
    if offset not in index:
        return ZERO
    i, digest = index[offset]
    sdata.seek(i * BLOCK)
    data = sdata.read(BLOCK)
    need(sha(data) == digest, f'{SURVEY_DATA} damaged at {offset}')
    return data


def survey(card, folder, log):
    """Read-only: read the whole area once, keep only its non-zero 64 KiB blocks."""
    folder = Path(folder)
    for name in (SURVEY_JSON, SURVEY_DATA, DELTA):
        need(not (folder / name).exists(),
             f'{folder / name} already exists: a survey must be fresh - use an empty folder')
    ident = card_identity(card)
    cap = MAX_SURVEY_DATA
    if (folder / KEEP_ALL).exists():
        cap = REGION_END - REGION_START + BLOCK     # never reached: keep everything
        log('KEEP_ALL: my decision - every non-zero block of the area is saved to the survey')
    area = hashlib.sha256()
    nonzero = []
    partial = folder / (SURVEY_DATA + '.partial')
    with open(partial, 'wb') as out:
        for pos in range(REGION_START, REGION_END, CHUNK):
            data = read_exact(card, pos, CHUNK)
            area.update(data)
            if data != ZERO_CHUNK:
                for k in range(0, CHUNK, BLOCK):
                    blk = data[k:k + BLOCK]
                    if blk != ZERO:
                        need(len(nonzero) * BLOCK < cap,
                             f'more than {cap} bytes of the Linux area are not zero: the area '
                             'is in use - STOP and ask (nothing was written to the card)')
                        out.write(blk)
                        nonzero.append({'offset': pos + k, 'sha256': sha(blk)})
            if (pos - REGION_START) % (2 * GiB) == 0:
                log(f'area {(pos - REGION_START) / (REGION_END - REGION_START):.0%}, '
                    f'{len(nonzero)} non-zero blocks so far')
        out.flush()
        os.fsync(out.fileno())
    need(card_identity(card) == ident, 'the card prefix changed during the survey')
    os.replace(partial, folder / SURVEY_DATA)
    total = (REGION_END - REGION_START) // BLOCK
    result = {'kind': 'nzeus-linux-area-survey', 'version': 1,
              'time_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'card_bytes': CARD_BYTES, 'region_start': REGION_START, 'region_end': REGION_END,
              'block': BLOCK, 'blocks': total, 'zero_blocks': total - len(nonzero),
              'area_sha256': area.hexdigest(), **ident,
              'data_file': SURVEY_DATA, 'data_sha256': file_sha(folder / SURVEY_DATA),
              'nonzero_blocks': nonzero}
    (folder / SURVEY_JSON).write_text(json.dumps(result, indent=2) + '\n')
    log('SURVEY_COMPLETE', total, 'blocks;', len(nonzero), 'not zero; area sha256', result['area_sha256'])
    return result


def compare_area(card, folder, log):
    """Read-only: [(offset, sha256 now)] of every block that differs from the survey."""
    s, _, index = load_survey(folder)
    need(card_identity(card) == {k: s[k] for k in ('first_mib_sha256', 'fat_boot_sector_sha256')},
         'Card identity prefix changed (first MiB or FAT boot sector): not the surveyed card')
    diff = []
    with open(Path(folder) / SURVEY_DATA, 'rb') as sdata:
        for pos in range(REGION_START, REGION_END, CHUNK):
            data = read_exact(card, pos, CHUNK)
            stored = [o for o in range(pos, pos + CHUNK, BLOCK) if o in index]
            if not stored and data == ZERO_CHUNK:
                continue
            for k in range(0, CHUNK, BLOCK):
                blk = data[k:k + BLOCK]
                if blk != surveyed(sdata, index, pos + k):
                    diff.append((pos + k, sha(blk)))
            if (pos - REGION_START) % (4 * GiB) == 0:
                log(f'compare {(pos - REGION_START) / (REGION_END - REGION_START):.0%}, {len(diff)} differ')
    return diff


def verify(card, folder, log):
    """Read-only: 'surveyed', 'installed' (exactly the plan's after content) or 'changed'."""
    diff = compare_area(card, folder, log)
    state = 'surveyed' if not diff else 'changed'
    if (Path(folder) / DELTA).exists():
        plan = load_plan(folder)
        if diff and [(b['offset'], b['after']) for b in plan['blocks']] == diff:
            state = 'installed'
        # Evidence for "Linux wrote only inside its area": the stock part is
        # still what the backup holds (unless a kernel was installed since).
        stock = hash_stock(card, log)
        log('STOCK_EXTENT_SHA256', stock)
        log('STOCK_EXTENT', 'SAME_AS_SNAPSHOT' if stock == plan['snapshot_sha256'] else
            'DIFFERS_FROM_SNAPSHOT (expected only if a kernel or file was changed on the FAT32 since)')
    log('REGION_STATE', state, len(diff), 'blocks differ from the survey')
    return state, diff


# --------------------------------------------------------------------- writing
def _plan_and_survey(card, folder):
    folder = Path(folder)
    plan = load_plan(folder)
    s, s_sha, index = load_survey(folder)
    need(plan.get('survey_json_sha256') == s_sha, 'the plan was not made from this survey')
    ident = card_identity(card)
    need(ident['first_mib_sha256'] == plan['first_mib_sha256'] == s['first_mib_sha256'],
         'Card identity prefix changed (first MiB): wrong card or changed card')
    need(ident['fat_boot_sector_sha256'] == plan['fat_boot_sector_sha256'] == s['fat_boot_sector_sha256'],
         'FAT boot sector changed: wrong card or changed card')
    return plan, index, ident


def install(card, folder, log):
    """Preflight ALL preimages, then write + read back every plan block."""
    folder = Path(folder)
    plan, index, ident = _plan_and_survey(card, folder)
    after = folder / AFTER_DATA
    need(after.stat().st_size == len(plan['blocks']) * BLOCK and file_sha(after) == plan['after_data_sha256'],
         f'{AFTER_DATA} does not belong to this plan')
    with open(folder / SURVEY_DATA, 'rb') as sdata, open(after, 'rb') as adata:
        # Preflight every changed block before writing any block.
        for i, b in enumerate(plan['blocks']):
            off = b['offset']
            need(sha(surveyed(sdata, index, off)) == b['before'], f'plan preimage at {off} is not what the survey saw')
            need(sha(read_exact(card, off, BLOCK)) == b['before'],
                 f'Card differs from the survey at {off} (stale survey or changed card) - nothing was written')
            adata.seek(i * BLOCK)
            need(sha(adata.read(BLOCK)) == b['after'], f'Image differs at {off}')
        log('ALL_PREIMAGES_VERIFIED', len(plan['blocks']))
        for i, b in enumerate(plan['blocks']):
            adata.seek(i * BLOCK)
            data = adata.read(BLOCK)
            write_verified(card, b['offset'], data, lambda got, d=b['after']: sha(got) == d)
            log('VERIFIED_BLOCK', b['offset'], BLOCK)
    need(card_identity(card) == ident, 'the card prefix changed during the install')
    stock = hash_stock(card, log)
    need(stock == plan['snapshot_sha256'], 'STOCK EXTENT DIFFERS FROM THE SNAPSHOT - stop and tell Claude '
         '(the snapshot before.img is the backup)')
    log('STOCK_EXTENT_UNCHANGED', stock)
    log('INSTALL_COMPLETE', len(plan['blocks']), 'blocks')
    return len(plan['blocks'])


def restore(card, folder, log):
    """Put the surveyed content back into exactly the blocks the install touched.
    Each block must hold either the installed or the surveyed content (an
    interrupted install is fine); anything else -> refused before any write."""
    folder = Path(folder)
    plan, index, ident = _plan_and_survey(card, folder)
    todo = []
    with open(folder / SURVEY_DATA, 'rb') as sdata:
        for b in plan['blocks']:
            off = b['offset']
            need(sha(surveyed(sdata, index, off)) == b['before'], f'plan preimage at {off} is not what the survey saw')
            now = sha(read_exact(card, off, BLOCK))
            if now != b['before']:
                need(now == b['after'], f'Card at {off} holds neither the installed nor the surveyed content '
                     '(Linux has written to the area?) - nothing was written; use region-reset')
                todo.append(b)
        log('ALL_PREIMAGES_VERIFIED', len(todo), 'to restore;', len(plan['blocks']) - len(todo), 'already as surveyed')
        if not todo:
            log('RESTORE_COMPLETE 0 blocks (already as surveyed, nothing written)')
            return 0
        stock = hash_stock(card, log)
        for b in todo:
            data = surveyed(sdata, index, b['offset'])
            write_verified(card, b['offset'], data, lambda got, d=b['before']: sha(got) == d)
            log('VERIFIED_BLOCK', b['offset'], BLOCK)
    need(card_identity(card) == ident, 'the card prefix changed during the restore')
    need(hash_stock(card, log) == stock, 'STOCK EXTENT CHANGED DURING THE RESTORE - stop and tell Claude')
    log('STOCK_EXTENT_UNCHANGED', stock)
    log('RESTORE_COMPLETE', len(todo), 'blocks')
    return len(todo)


def reset(card, folder, log):
    """After Linux has used the area: make the WHOLE area equal to the survey
    again. Reads the whole area, then writes only the blocks that differ (no
    44 GiB of zeros). Allowed only on the surveyed card (identity hashes) whose
    header blocks hold this plan's header or the surveyed content."""
    folder = Path(folder)
    plan, index, ident = _plan_and_survey(card, folder)
    with open(folder / SURVEY_DATA, 'rb') as sdata:
        for off in (REGION_START, HEADER2):
            now = read_exact(card, off, BLOCK)
            need(sha(now[:512]) == plan['header_sha256'] or now == surveyed(sdata, index, off),
                 f'the block at {off} holds neither this plan\'s NZEUS header nor the surveyed content: '
                 'not our Linux area - nothing was written')
    diff = compare_area(card, folder, log)
    log('RESET_PLAN', len(diff), 'blocks differ from the survey')
    if not diff:
        log('RESET_COMPLETE 0 blocks (already as surveyed, nothing written)')
        return 0
    stock = hash_stock(card, log)
    runs = []
    for off, _ in diff:
        if runs and runs[-1][0] + runs[-1][1] == off and runs[-1][1] < CHUNK:
            runs[-1][1] += BLOCK
        else:
            runs.append([off, BLOCK])
    with open(folder / SURVEY_DATA, 'rb') as sdata:
        for off, n in runs:
            data = b''.join(surveyed(sdata, index, o) for o in range(off, off + n, BLOCK))
            write_verified(card, off, data, lambda got, d=data: got == d)
            log('VERIFIED_RUN', off, n)
    need(card_identity(card) == ident, 'the card prefix changed during the reset')
    need(hash_stock(card, log) == stock, 'STOCK EXTENT CHANGED DURING THE RESET - stop and tell Claude')
    log('STOCK_EXTENT_UNCHANGED', stock)
    log('RESET_COMPLETE', len(diff), 'blocks')
    return len(diff)


def check_folder(folder):
    """Files only: plan rules + the plan's data files belong to it."""
    folder = Path(folder)
    plan = load_plan(folder)
    _, s_sha, index = load_survey(folder)
    need(plan.get('survey_json_sha256') == s_sha, 'the plan was not made from this survey')
    after = folder / AFTER_DATA
    need(after.stat().st_size == len(plan['blocks']) * BLOCK and file_sha(after) == plan['after_data_sha256'],
         f'{AFTER_DATA} does not belong to this plan')
    with open(folder / SURVEY_DATA, 'rb') as sdata:
        for b in plan['blocks']:
            need(sha(surveyed(sdata, index, b['offset'])) == b['before'],
                 f'plan preimage at {b["offset"]} is not what the survey saw')
    return plan


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != 'check':
        sys.exit(__doc__)
    try:
        p = check_folder(sys.argv[2])
    except Refused as e:
        print('REFUSED:', e)
        sys.exit(2)
    # One line per fact, so the PowerShell session script needs no JSON parser
    # (Windows PowerShell 5.1's ConvertFrom-Json fails on files over 2 MB).
    print(f'PLAN_OK blocks={len(p["blocks"])} bytes={len(p["blocks"]) * BLOCK} '
          f'first={p["blocks"][0]["offset"]} end={p["blocks"][-1]["offset"] + BLOCK} (all inside the Linux area)')
    print('SURVEY_AREA_SHA256', p['survey_area_sha256'])
    print('BLKDEVPARTS', p['blkdevparts'])
