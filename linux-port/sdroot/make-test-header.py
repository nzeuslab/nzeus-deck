#!/usr/bin/env python3
"""Make a REAL NZEUS Linux-area header for PC tests of nz-sdboot.

Uses linux-port/rootfs/region-tool/nz_region.py (imported read-only:
compute_layout, make_header, parse_header, fs_uuid) so the test header is
byte-for-byte what `nz_region.py build` writes into nzhdr/nzhdr2.
Writes only into --out (an ordinary folder): header.bin (512 bytes) and
layout.json (the layout, sysfs-style sector numbers and the ext4 UUIDs).
Never opens a device.

  python3 make-test-header.py --out DIR [--cid HEX32] [--card-bytes N] [--fat-end N]
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'rootfs' / 'region-tool'))
import nz_region  # noqa: E402  (read-only use; the region tool is owned elsewhere)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--cid', default=nz_region.TEST_CARD_CID)
    ap.add_argument('--card-bytes', type=int, default=nz_region.CARD_BYTES)
    ap.add_argument('--fat-end', type=int, default=nz_region.SNAPSHOT_BYTES)
    ap.add_argument('--first-mib-sha256', default='00' * 32)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    layout = nz_region.compute_layout(args.card_bytes, args.fat_end)
    uuids = {n: nz_region.fs_uuid(n, args.cid) for n in ('nzroot', 'nzdata')}
    header = nz_region.make_header(layout, args.cid, args.first_mib_sha256, uuids)
    parsed = nz_region.parse_header(header)          # round trip with the tool's own parser
    assert parsed['cid'] == args.cid.lower() and len(parsed['parts']) == 5
    assert [p['start'] for p in parsed['parts']] == [p['start'] for p in layout['parts']]
    (out / 'header.bin').write_bytes(header)
    info = dict(cid=args.cid.lower(), card_bytes=args.card_bytes, card_sectors=args.card_bytes // 512,
                blkdevparts=layout['blkdevparts'], uuids=uuids,
                parts=[dict(num=p['num'], name=p['name'], ro=p['ro'], start_sectors=p['start'] // 512,
                            size_sectors=p['bytes'] // 512) for p in layout['parts']])
    (out / 'layout.json').write_text(json.dumps(info, indent=2) + '\n')
    print('TEST_HEADER_READY', out / 'header.bin', layout['blkdevparts'])


if __name__ == '__main__':
    main()
