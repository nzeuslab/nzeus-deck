# Region tool (PC side) — plans the NZEUS Linux area; the card is written only by card_io.py

`nz_region.py` works only on **image files** (it refuses `/dev/...` and
`\\.\...` paths and anything that is not a regular file) and has **no install
action**. The real card is read and written only by
`../../card-test/card_io.py` (`region-*` actions, profile `linux-area`,
elevated, Windows). All of card_io's Linux-area rules live in
`../../card-test/region_io.py`, and **the self-test below runs exactly that
module** against a fake card file, so the Windows code path is tested on the PC.
Why and how it fits the card workflow: `../PLAN.md` sections 2–4 and 12.

Guided Windows session (what I run): `../../purelinux/session/sdroot-install.ps1`
(see `../PLAN.md` section 12). It calls `wsl-region.sh` for the WSL part.

Run in WSL (Python 3.12+, e2fsprogs; the self-test also needs dosfstools + mtools):

```
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py layout              # numbers + blkdevparts line
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py inspect before.img   # read sector 0 + FAT32 from a snapshot
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py survey --source <full card image FILE> --out <dir>
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py build --snapshot before.img --rootfs-ext4 ~/nzeus-rootfs-debian/out/rootfs.ext4 --out ~/nzeus-region/<name>/build
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py plan --snapshot before.img --snapshot-json snapshot.json --survey <card folder> --build <build dir> [--out <card folder>]
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py check <card folder>
python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py selftest --workdir <scratch folder> [--rootfs-ext4 rootfs.ext4]
bash    ports/r36s-hichip/rootfs/region-tool/wsl-region.sh check|plan [rootfs.ext4]   # used by sdroot-install.ps1
python  ports/r36s-hichip/card-test/region_io.py check <card folder>                # Windows or WSL, files only
```

The real survey of the card is `card_io.py region-survey --profile linux-area`
(same code, same files); `nz_region.py survey` is for fake card images.

## Files (card folder `ports/r36s-hichip/rootfs/card-region` = junction to `<work folder>\card-region`)

| File | Made by | What |
|---|---|---|
| `before.img` + `snapshot.json` | `card_io.py snapshot` | the usual backup of the stock part (first 16,357,785,600 B) |
| `region-survey.json` | `card_io.py region-survey` | the area as it was: offset + SHA-256 of every non-zero 64 KiB block, SHA-256 of the whole 46,682,603,520-byte area, first-MiB and FAT-boot-sector hashes, time |
| `region-before.blocks` | `card_io.py region-survey` | those non-zero blocks back to back (empty for an empty area). Not a sparse image on purpose: NTFS files are not sparse by default (a 44 GiB "sparse" file would really take 44 GiB on D:) and WSL cannot see holes through `/mnt/d` |
| `delta.json` | `nz_region.py plan` | the sector plan, `prepare_card.py` style: absolute `offset`, `bytes` (64 KiB), `before`/`after` SHA-256, plus identity hashes, the snapshot hash, the survey file hash, the layout and the `blkdevparts` line |
| `region-after.blocks` | `nz_region.py plan` | the new content of every plan block, in plan order (what `region-install` writes) |
| `build.json`, `nzroot-initramfs.conf` | `nz_region.py build` (copied) | layout, CID, UUIDs, header hash, root image hash; values for the boot check |
| `region-*.log` | `card_io.py` | evidence of each card action |

The candidate itself (`region-candidate.img`, a 44 GiB **sparse** file) stays
on the WSL disk (`~/nzeus-region/<UTC time>/build/`); `build` refuses folders
under `/mnt/`.

## card_io.py region actions (profile `linux-area`; rules in `region_io.py`)

| Action | Writes? | What |
|---|---|---|
| `region-survey` | no | reads the whole area once (20–40 min); refuses a folder that already holds a survey or plan; refuses (stop and ask) if more than 1 GiB of the area is not zero |
| `region-install` | area only | plan rules → survey + after-data files belong to the plan → first MiB + FAT boot sector match plan and survey → **every** `before` hash checked on the card (stale survey / changed card → refused, nothing written) → write + fsync + read back each block → identity again → **whole stock part re-read and compared with the snapshot hash** |
| `region-restore` | area only | way back right after an install: only the blocks the install touched, back to the surveyed content (zeros or the saved block). Each block must hold the installed or the surveyed content (an interrupted install is fine); anything else → refused before any write. Stock part hashed before and after |
| `region-reset` | area only | way back after Linux has used the area: both header blocks must hold this plan's header or the surveyed content; reads the whole area, then writes the surveyed content only to blocks that differ (not 44 GiB of zeros), with readback. Stock part hashed before and after |
| `region-verify` | no | reads the whole area: `surveyed`, `installed` or `changed` |

## Safety rules built in

- Layout: FAT32 must be the only partition, start at 1 MiB and end at least
  64 MiB before 16 GiB; the area ends at the last 4 MiB boundary of the card.
  `plan` also checks that the computed area is exactly `region_io.py`'s fixed
  `[16 GiB, 60,904 MiB)` (card_io never trusts the plan's numbers).
- Plan: snapshot must match `snapshot.json`; sector 0 must be in the stock
  scrambled ("closed") state; the survey must come from the same card state
  (first MiB + FAT boot sector); every block one aligned 64 KiB block in
  `[16 GiB, 60,904 MiB)` and above both the FAT end + 64 MiB and the
  16,357,785,600-byte snapshot extent; blocks sorted, non-overlapping; plans are
  never overwritten.
- **Size limit 2 GiB** (was 512 MiB; `region_io.MAX_PLAN_BYTES`, also the
  default `--max-bytes`). The plan holds the root file system's used blocks
  plus ext4 metadata: a Debian minbase root with python3 uses roughly
  0.3–0.7 GiB, the empty 39.5 GiB `nzdata` adds a few MiB (its journal and
  inode tables are zeros, which are not written). 2 GiB leaves room for a
  bigger root while still refusing runaway plans (a candidate that lost its
  holes, or an area that reads 0xFF, would be ~44 GiB). `plan` stops early
  when the limit is passed.
- Preimages come from the survey: a block that was not zero is planned with
  its real old content as `before`, so install verifies it first and restore
  can put it back.

## Self-test

`python3 nz_region.py selftest --workdir <scratch>`: fake sparse card of
exactly 63,864,569,856 bytes with a scrambled sector 0 and a FAT32 of
31,946,752 sectors at 1 MiB holding `cubegm/` files. Then inspect → layout →
**survey with card_io's code** → build → plan → **install, verify, restore,
reset with card_io's code (`region_io.py`) on copies of the fake card**, and
the failure cases. Every card action in the self-test opens only a card-sized
regular file inside the self-test folder.

Checked: first 16,357,785,600 bytes unchanged (SHA-256) after install, restore
and reset; no data in the gap or after the area; both ext4 clean; main = backup
header; stock file readable; verify says `installed`; restore writes exactly
the touched blocks, a fresh survey afterwards gives the same whole-area SHA-256,
a second restore has nothing to do; an interrupted install (half written) is
restored; after simulated Linux writes restore refuses and changes nothing,
reset writes only the differing blocks and verify says `surveyed`; junk in the
area is surveyed, planned as preimage, and put back by restore. Refusals:
stale survey, changed first MiB, changed FAT boot sector, card of another size
and a smaller card (area end unreadable), changed `region-after.blocks`, changed
survey file, damaged `region-before.blocks`, second survey into the same
folder, reset of an area without our header, survey cap, blocks in the FAT32 /
gap / after the area / unaligned / overlapping, plan for another area, plan
over 2 GiB (card_io rules and `--max-bytes`, stopping early), edited partition
line, build folder under `/mnt/`, device paths, changed snapshot, second
partition, too-small card.

Results: see `../PLAN.md` section 12 (test log).
