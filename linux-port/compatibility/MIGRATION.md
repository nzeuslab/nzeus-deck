# Modern Linux migration: evidence and next work

Updated 2026-10-07 after the successful modified-stock SD boot and the Deck
publishing handoff review. The target is a usable modern-Linux handheld, with
more usable RAM, native networking, standard terminals, Linux tools, and working
display, input, sound, power and USB peripherals. An SDK 4.4 rebuild is a possible
diagnostic aid, not the destination or a prerequisite for every modern-kernel task.

## Completed in this step

- Cross-compiled an ABI audit using the existing MIPS SDK compiler. The SDK audio
  structure is 608 bytes and AUDDEC_INIT is `0x82600301`; a 632-byte structure gives
  `0x82780301`. The 16-byte audio packet header is confirmed. See `audio-abi.json`.
  The console accepting 632 bytes comes from the publishing handoff, not a new
  hardware experiment in this step.
- Rebuilt modern Linux 7.2.0-rc5 with a conservative board tree and temporary
  `clk_ignore_unused`. Board and artifact checks pass, including the rescue shell
  under QEMU, appended DTB placement, memory limits and exact initramfs contents.
- Added `../package-modern-sd.py`: gzip uImage plus the factory ChaCha20 wrapper,
  using load/entry `0x81000000`. Output remains PC-only and hardware-unverified.
  It decodes and expands back to the exact modern image and attached DTB.
- Checked the factory Linux call: `0x89dadbb4..0x89dadbc4` supplies `a0=-2`,
  `a1=DTB`, `a2=0`, `a3=0`, matching the UHI convention. This is from the actual
  decoded bootloader, not only the SDK. The called helper at `0x89dc04a4..0x89dc0534` saves and masks interrupt
  registers, including writes to `0xb8800038`, `0xb880003c`, and `0xb8800044`.
  See `factory-entry-evidence.txt`. SDK image loading calls `flush_cache`
  (`common/bootm.c:338`), and the actual factory call at `0x89dad25c` to `0x89da621c` now confirms
  D-cache writeback/synchronization for the loaded image. See the appended
  instruction ranges and `../card-test/TEST-PLAN.md` for the controlled-test review.

No console files, running programs, settings or flash were changed in this step.

## Why rebuilding the SDK is not a compatibility guarantee

In `/home/nzeus/r36s/kernel/linux-4.4.186/drivers/hcdrivers/avp-proxy/avp-proxy.c`:

- `avp_ioctl_preproc`, lines 328-343, matches the exact AUDDEC_INIT number.
- `avp_ioctl_postproc`, lines 363-385, also matches it before acquiring the audio
  shared-memory write handle with KSHM_WRITE_HDL_ACCESS.
- `default_ioctl`, lines 536-565, allocates the command's encoded size, forwards
  the command to AVP, and performs that postprocessing.

Consequently, sending the newer 632-byte command through an unchanged SDK proxy
would miss the older exact-match handling. This is a source-level incompatibility
for a proposed SDK replacement; it does NOT establish the cause of the existing
frontend's silence on factory Linux. The full newer structure layout is unknown;
padding the old structure by 24 bytes has not established field compatibility.

The Deck frontend's `hwsound.c` still uses SND_IOCTL_XFER. The handoff records that
path as unsuccessful for playback and reports incomplete `/dev/auddec` experiments.
Keep that release-sound investigation separate from claims of modern ALSA support.

## Factory boot into modern Linux

```text
Unmodified NOR loader
  -> factory AVP firmware and selected SD tree
  -> encrypted SD kernel file (format proven with stock marker)
  -> modern self-decompressor at 0x81000000 (not yet booted)
  -> appended modern board tree + built-in rescue filesystem
  -> USB ACM terminal, then native networking tests
```

The modern `arch/mips/include/asm/bootinfo.h:123-137` chooses a valid appended DTB
before the bootloader-provided tree. `arch/mips/boot/compressed/decompress.c:106-124`
copies it to the uncompressed kernel's expected location. Therefore the candidate
can carry a separate modern tree without replacing `Bubbles.scr`, whose selected
properties remain important to factory AVP startup. This is verified source behavior,
not hardware proof of this boot combination.

The candidate decompressor's physical memory ends at `0x01978ab0`, below mirrored
factory bootmem start `0x01da0000`; the packer rejects overlap. The uncompressed
kernel and an extra DTB allowance fit below the decompressor at 16 MiB. The first
Linux RAM range remains `0..0x02f91e50`; no additional RAM has been reclaimed.
These layout checks do not establish that all live DMA/shared-memory users are safe.

The modern clock driver registers display, audio and DMA gates
(`drivers/clk/clk-hc16xx.c:293-342`). Generic unused-clock cleanup may disable gates
that the separate firmware still needs. `clk_ignore_unused` prevents that cleanup
for bring-up. It is temporary: it neither stops AVP nor resolves concurrent device
ownership, and should be removed after proper drivers manage those clocks.

## Remaining work, ordered by useful hardware evidence

| Milestone | Work | Pass condition |
|---|---|---|
| Modern rescue boot | Finish cache/interrupt and AVP/shared-controller handoff review; use reversible SD replacement and saved original | Modern version reported by USB terminal; repeated boots; original SD file restores working NZEUS |
| Native Linux basics | USB network function, PTYs, read-only SD integrity checks, then separate writable root filesystem | Console/PC exchange packets through kernel TCP/IP; PTY program works; files match hashes |
| Usable handheld | R36S display/panel, input, power-hold, battery, audio; migrate Deck hardware interfaces | Screen/buttons/sound and clean shutdown verified on actual board |
| USB host and wireless | Validate HiChip host-controller behavior, then dongle driver/firmware and normal network tools | USB peripheral tests, Wi-Fi reconnect and HTTPS, Bluetooth profiles tested individually |
| More application RAM | Resolve AVP ownership or replace its services; map DMA and memory before expanding | No overlap/aliasing, memory stress, sustained hardware use and repeated cold boots |
| Full software environment | MIPS-compatible rootfs and package/build workflow, Python 3, curl, SSH, git, vim/tmux | Programs run on device using standard networking and terminals |
| Daily console | Performance, suspend/power if supported, stability, rollback and user acceptance | Standalone cold boot with PC disconnected and complete Deck hardware test plan |

Memory and hardware-driver work can overlap after the rescue baseline is established.
"Full features" means validated features this board can support; not every Linux
package supports 32-bit MIPS and kernel class drivers do not replace missing SoC
drivers. Do not promise a RAM total, WPA3, Bluetooth audio or modern Node.js in advance.

## Next concrete investigation

The factory entry arguments, cache writeback call and interrupt masking are
now traced. The rescue build uses CPU-local Count/Compare timers and disables
shared timer 4, SD/MMC, GPIO/input and PWM/backlight. SDK source uses timer 5 for
scheduler ticks and timer 3 for high-resolution time; it initializes timer 4,
which alone is not evidence of continuous use. The conservative CPU-timer choice
avoids the shared south timer interrupt. See `r36s-cpu-timer.patch` and
`../apply-port-patches.py` for the pinned, repeatable source adjustment. The
next step is the bounded USB-only hardware experiment in `../card-test/TEST-PLAN.md`. A cold start may leave a static factory logo even
if Linux starts, so USB enumeration and a modern-version shell are the pass signal.

## Sources and reproducibility

- Modern source pinned at `da5f1a22ef2ff5c46aa3f750fa10825813970b09` in
  `/home/nzeus/r36s-linux-port-20261007`.
- [Upstream driver status](https://github.com/SiarheiVolkau/linux-hichip/blob/da5f1a22ef2ff5c46aa3f750fa10825813970b09/Readme.md),
  rechecked online 2026-10-07: display/audio unfinished, USB host not supported as
  a working feature. Some host-mode code exists; that alone is not a passing driver.
- Factory instruction evidence: `../boot-analysis/bootloader-disasm.txt` and its
  pinned binary/hash report. SDK `bootm_os.c:75-90` independently matches the call.
- `wsl -d Ubuntu -- python3 linux-port/compatibility/audit-audio-abi.py`
- `wsl -d Ubuntu -- bash linux-port/validate-board.sh`
- `wsl -d Ubuntu -- bash linux-port/build-experimental.sh`
- `python linux-port/package-modern-sd.py`

The public release and GPL source archive were not regenerated or published.
