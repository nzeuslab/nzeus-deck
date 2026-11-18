# Modern Linux: current baseline and remaining work

Updated 2026-10-14. This page supersedes the dated status paragraphs in the
port READMEs. Those paragraphs and older display handoffs describe earlier tests.
Evidence below comes from saved hardware logs and my own confirmations unless
explicitly marked as a PC check; it is not a claim of a new hardware retest.

**Current modern-only follow-up, October 13:** the daily hardware configuration
is the tested 900 MHz hardware-FPU kernel described in my lab notes;
older nofpu paragraphs below are historical. Health's unsupported previous-start
row/stock log reader are removed and installed on BLACK. 32 managed paths and
21 protected hashes verified, restart watcher kept the app; live screen inspected.
Real GUI/vault/helper/SSH/PTY connection regression passes against loopback
OpenSSH (no live Claude account test). Tools 345/345, GUI 709 checks,
helper/support 18 each, Debian 278, MIPS/adapter/emulator checks pass.

Fresh generic root image `9824ce92...` (October 14) includes current modern app, NetSurf,
examples and emoji: 222 packages, 1,533 matching managed paths, privacy clean,
none of my authorized keys or user data. Real-image fake-card recovery test 70/70.
Not yet boot-tested on a fresh card. Read `rootfs/debian/FRESH_CARD_2026-10-14.md`.
637 old-system paths plus 79 obsolete Web screenshots are purged from active
local/GitHub branch history, with private recovery bundles. Fresh real-card
baseline completed read-only October 13: verified stock extent `135c88ad...`
and Linux area `8fdf12c8...`, including every saved block and reconstructed-area
hash. No card writes or generic-image installation occurred. Old card-region
preimages are stale. Archived helper-copy directories remain
because native removal/automatic approval blocked their physical deletion.
See `../reports/continuation/modern-cleanup-20261013.md`.

**October 14 app follow-up:** WhatsApp achieves signed remote authentication
and I confirmed chats appear over the phone hotspot. A clearer SSH-error
helper is PC-tested (14 transport cases, real loopback SSH and 18 helper tests)
and signed/staged, not installed on the console. Generic-image builds now keep
`NZ_NO_AUTHORIZED_KEYS=1` through the sudo handoff; its regression first failed
and now passes. Car follow-up research is in
`../docs/car-feature-next-step-20261014.md`: saved captures lack usable module
identification and predate the installed GM discovery. I will scan later.
Then home Wi-Fi failed: only the public PC address was
saved. On the verified live console, the home address was set to the PC's LAN
address and the public address retained as `claude_pc_public`. Original settings
backed up; app, encrypted WhatsApp key and seven boot files unchanged; app
restart passes. A live status request from the unlocked console over home Wi-Fi
authenticates successfully and returns `state=ready`. I then
confirmed received photos open and voice notes play on the console. Arabic/emoji
rendering and sending remain separate checks.

**Historical fresh-card image, 2026-10-12 (superseded by October 13 above):**
`rootfs.ext4` sha256 `f9790acc46e129b92d90e2750fe8edeeeade33228014b08def141d3e797655ce`,
built 16:53 UTC with `build-debian.sh --reuse --packages` from commit `ac6300c`
(Deck `nzeus-modern` `3bf416e9...` includes ac6300c). 197 packages: adds
poppler-utils + 27 deps, alsa-utils, firmware-realtek, wireless-regdb, usbutils
(0 upgrades); overlay has nzeus-ram/usbnet/voldown, my local time zone, my public key,
all 21 Deck helpers. Image + provenance + build log: `<work folder>\debian-image-20261012`
(also WSL `~/nzeus-rootfs-debian/out`). Provenance (`rootfs/debian/manifest/debian-rootfs-provenance.json`,
`provenance.py`): 97 managed paths, all byte-identical to their repo sources, 0 unmapped.
Builder's `repo_tracked_changes=yes` is a WSL junction artefact (card-keys/card-sdro snapshot
links to D:); no image input changed. Privacy scan (`privacy-scan-image.sh`) clean; the SSH
key comment names me, so the image stays private. PC tests: rootfs 269/269 (one new
helper check), fake-card recovery 70/70, real-image region self-test 70/70, Deck self-test 224
pictures. What differs from the live card (docs only): `manifest/what-differs-20261012.md`
(Deck app + 23 helpers same as the 19:56 signed update, 70 paths need a live hash;
image adds firmware-realtek, wireless-regdb, usbutils, libusb as packages, which the
live card got by hand or not at all). Dry-run sync planner:
`rootfs/debian/sync-managed-plan.py` (never runs anything). No card, console or NOR access.

**Vehicle support preparation installed 2026-10-12:** Car now saves private
module-identification evidence after completed scans and has manual vehicle
selection, signed offline owner guides and a verified HTTPS download pipeline.
One conditional 2016 US Silverado owner-menu guide is installed. No production
catalog endpoint is configured; no actual feature toggles, verified coding
profiles or write/restore controller exist. Selection and responding modules
do not establish feature compatibility. New identifiers still need a real-car
test. PC: 640 GUI checks, 18 support trust/transport tests, 13 helper tests pass;
final label polish also passes static MIPS, 35 adapter checks and 224-picture
emulator self-test, zero warnings. Signed app `b5e91a0d...` (2,440,584 bytes)
and final helper pass 24-path readback and watched restart on BLACK. Hardware
screenshots show readable actions/guide and normal home. Test selection removed;
reports/settings/kernel/boot assets unchanged, temporary update server stopped.
See `../reports/continuation/car-support-20261012.md`.

**PDF Reader installed 2026-10-12:** Files now opens PDF reports. L/R turns
pages, A cycles zoom, D-pad scrolls, X rotates through four orientations, B
returns to Files. I saw soft zoom in the first installed reader;
the final version renders near-native 1238 x 1750 report pixels. All five real
English/Arabic pages pass native rendering in 3.04-3.75 seconds. Hardware
screenshots verify zoom, scrolling, rotation and page turns; Arabic stays RTL.
Poppler and 27 dependencies are installed on the SD Debian, no packages upgraded
or removed, dpkg audit clean. Future image packages.txt includes Poppler.
The full GUI run passes 634 checks; after numeric render-limit tuning, all 16
focused reader/35 adapter checks and MIPS/self-test pass, zero compiler warnings.
Signed app `172df492...` (2,432,824 bytes) is installed; 23-file readback and
60-second watcher pass. Rollback retained; original reports, updater settings,
kernel and protected boot assets unchanged. Temporary update server stopped.
I accepted the updated reader ("perfect") after the sharpness/rotation update. See `../reports/continuation/pdf-reader-20261012.md`.

**Car field recovery installed 2026-10-12:** both real Chevrolet 2016
(my car: Silverado) PDFs pass structural and visual checks; Arabic is joined/RTL.
The warning is an incomplete scan, not a PDF failure. English and Arabic were
separate scans. OBDLink EX r2.7.1 is confirmed by the field reports; all-module
coverage is not. Same-port read failure and rejected/timed-out restoration were
reproduced in simulations and now recover while preserving the completed scan.
All 618 GUI and 35 identity/restore checks pass; static MIPS/self-test passes,
zero compiler warnings. Signed app `7119305f...` (2,425,720 bytes) is installed;
23-path readback and 60-second watcher pass, I confirm home and D-pad.
Previous app retained for rollback; real reports, original updater settings,
kernel and protected boot assets unchanged. Temporary update server stopped.
Exact street trigger remains unknown; real-car repeat is planned tomorrow.
Files lacked a PDF handler at this earlier stage; the reader above supersedes that limitation.
See `../reports/continuation/car-field-recovery-20261012.md`.

**Earlier OBDLink identity continuation 2026-10-12:** real USB logs confirm an FT-X
adapter bound by ftdi_sio to ttyUSB0; my photo confirms the ELM-compatible
reset response, with no vehicle connected. This proves adapter communication,
not an exact EX model or car coverage. The app now identifies STI/STDI while
waiting for a car and shows the model in no-car status. All 27 focused identity
checks, 612 GUI checks and the static MIPS hardware/audio/emulator checks pass,
with zero compiler warnings. Signed one-app update e043c1d0... is installed on
BLACK; all 23 files passed readback, running PID 7760 matches, and the 60-second
watcher committed. Home screenshot is inspected; my post-restart control
confirmation and actual EX/model/car tests remain pending. Temporary server
stopped, original update configuration/kernel/boot assets unchanged. I have
a Silverado, uncertain year 2013-2016: resolve from VIN if available, do not
guess manufacturer-specific definitions. See
`../reports/continuation/obdlink-identity-20261012.md`. No vehicle writes performed.

**USB-driver diagnostic build #12 boot verified (October 12):** live USB SSH
confirms Debian 12.15, expected card, nofpu, 117472 KiB MemTotal and installed
container `40f7f56e...`. Confirmed on the console: BLACK home screen, D-pad and Terminal.
CH341/CP210X/FTDI/PL2303 and CDC ACM drivers are registered. Actual OBDLink EX
recognition/car scans remain pending. App/libapt and protected boot assets
match the prior baseline. Native apt passed 20/20 basic checks and 10/10 install simulations, with no
package installation or library overrides. This
combined driver/ALSA/nofpu build is diagnostic, not a proven FPU fix. The
113-block reader install passed all preimage/readback checks. Exact build-#9
recovery is retained; recovery after this boot needs a NEW snapshot.
See `diagnostics/apt/usbserial-install-results-20261012.json`.

**Car usability installed on BLACK (October 12):** signed one-app update
`324efff8...`, 2,425,048 bytes. All 23 managed paths passed live preflight and
installed readback, with verified PC backup and separate rollback retained.
New PID 1920 survived the 60-second watcher; I confirm home and controls.
Hardware screenshots show readable guide, working scroll, tools and honest
feature-status page. Python 3.11.2/PTY and helper checks pass. Boot assets,
kernel and original update configuration are unchanged; loopback server stopped.
Actual EX recognition/car reports remain pending. Feature coding and support
pack downloads remain unavailable. Requested vehicle ranges are development
targets, not validated coverage. See
`../reports/continuation/car-install-20261012.md`.

**Earlier USB-adapter PC preparation (superseded by the card write above):** I identified
OBDLink EX and asked for broad car coverage. A separate package combines the
newer USB-serial/ALSA kernel with the software-FP diagnostic fallback. PC build,
container roundtrip, exact DTB substitution, recovery-copy and refusal checks
pass; card-delta tests pass 10/10. This new combination has not booted. The exact
original build-#9 container is retained for recovery. Current live kernel/app
hashes remain unchanged, nofpu is absent, and battery reports 78%. Fresh reader
identity/snapshot and observed boot are next. EX enumeration, real-car scans
and reports remain unverified. Standard OBD-II coverage must not be described
as all manufacturers' modules or every car. Details: diagnostics/apt/README.md.

**Priority incident (October 12):** inserting a cable into DC/charging during
the boot logo repeatedly causes display corruption; one attempt requested a
TF card. At first, battery-only boots failed for me. This also
happened with OTG empty. This initially held the application update on GREEN. A fresh card
backup has completed and its full saved-file hash is verified. The whole stock
boot area is unchanged. Root's read-only filesystem check reports no errors.
Data's initial check returns 0 despite summary-count warnings with journal
recovery skipped. On a separate extracted copy, journal-only replay clears
those warnings and a subsequent full read-only check reports no errors. No
original-card repair or write occurred. Note: only the white
USB-C connection reproduces the fault and has never shown charging; USB-A does
show charging. Keep the white connection out of the recovery test.
I then confirmed a cable-free boot through the NZEUS logo to
the working home screen. Live USB serial checks then confirmed the expected
card CID, Debian 12.15, Linux build #9, 117568 KiB RAM, running NZEUS process,
and unchanged installed executable/kernel/AVP/stock boot-file hashes. Root and
data mounted successfully; this boot log contains no SD I/O or ext4 errors.
I connected USB-A charging to DC and PC data to OTG. Live power state
reports Charging, charger online, capacity 0% (Critical); the percentage is
roughly calibrated and charging current was not measured. Hold updates and
restart tests until the battery has recovered. The white-USB-C fault's root
cause remains unknown. See `diagnostics/charging-boot/README.md`.
The subsequent power recheck could not start because the USB gadget disappeared;
whether the console powered off or only lost USB is not yet confirmed.

**Current connected unit - BLACK (October 12):** I moved the same modern SD
card to the black console and it works. Two fresh USB
serial commands completed with exit 0: Debian 12.15, Linux build #9, 117568 KiB
RAM, expected card CID, running NZEUS, and baseline app/kernel/AVP/Bubbles/dtb
hashes all confirmed. Both Linux partitions mounted; this boot log shows no
SD I/O/ext4 errors. Battery reports 46%, Discharging, charger offline. Modern
Linux testing can continue on BLACK; GREEN is not required. Color is my own
identification: the card CID and software-defined USB identity do not uniquely
identify the physical console. The black board revision has not been read.
Local captures: `diagnostics/uart-usb-fix/usb-command-20261011T213127.txt` and
`usb-command-20261011T213133.txt` (UTC names, October 12 local; not committed).
The green charging fault remains unresolved. The application was subsequently
updated on BLACK as described below.

**Application installed on BLACK (October 12):** I approved the signed
11-file update after a fresh 23-path baseline comparison and verified PC/card
backups. Every installed hash/size/mode passed readback. Running app SHA-256
is `4ca5f0ddd22546c9b8aa9e247ad7edd03833bf12a12d8c928e768a86195665d2`.
The app passed its 60-second restart watcher, cleared pending state, and I
confirmed the home screen and controls. Native Terminal Python/PTY and
helper syntax/fetch checks passed. Media/ALSA actively played a verified WAV;
audible confirmation and prolonged playback remain pending. Kernel/boot-file
hashes are unchanged. See `../reports/continuation/modern-install-20261012.md`
for evidence, rollback and a separate native Python test-generator exception.

## Working baseline

- Green HC16E3100V20 test console, Linux 7.2.0-rc5 based on upstream commit
  `da5f1a22ef2ff5c46aa3f750fa10825813970b09`, Debian 12 bookworm/mipsel.
- Boots from SD without a PC. Root is partition 3 (ext4), persistent `/data`
  is partition 4 (ext4), original FAT partition 1 is mounted read-only.
- About 115-117 MiB usable RAM after the approved AVP firmware stand-in removes
  the second processor's reservation. Tests show the upper memory mirrors the
  lower 128 MiB; the physical reason is not conclusively established.
  Increasing the DT memory size is not a supported fix.
  October 14 upstream evidence explicitly names our R36S V2.7/RAM part and
  describes moving the flash window to expose 192–256 MiB. This reopens the
  cause of our +128 MiB alias without establishing additional usable memory;
  `../research/ram-remap-upstream-2026-10-14.md`. Current RAM settings are unchanged.
  Subsequent live preflight found the upstream bit already enabled (0x18800220
  = 0x01000010); aliasing persists. The saved bootloader's identical setter and
  call were verified against raw bytes. This operation is not a new fix to apply.
  An eight-check PC-tested private scratch probe was prepared; its native
  preparation refused because its bounded allocation batch found no suitable
  owned low page. No upper pattern test, mapping write or reboot occurred.
  Eight app/boot hashes remain unchanged. A separately reserved scratch-page
  diagnostic is the next proposed experiment, not a completed RAM upgrade.
- Display with correct colors, buttons, brightness, battery/charger readings,
  power-off and restart have hardware evidence.
- USB device networking/SSH and switching to USB host mode have hardware
  evidence. RTL8821CU dongle Wi-Fi, DHCP and internet access were tested.
- I heard native ALSA speaker playback, including WAV/MP3,
  44.1/48 kHz and voice. On October 12 I also heard headphones with
  the speaker muted, then confirmed the manual speaker switch works perfectly.
  FN + Vol+ and Settings > Sound > Speaker provide the installed switch;
  automatic plug detection was investigated and was unreliable in those tests.
  Headphone/speaker testing is complete for this manual operating mode; do not
  list it as pending. See the audio-history correction in
  `../reports/continuation/console-progress-20261014.md`.
  BLE mouse pairing/pointer input were tested.

Evidence: `purelinux/session/logs/sdroot-first-boot-20261010-210955.txt`,
`purelinux/session/logs/host-update-alsa-20261011-212757.txt`,
`../research/ddr-geometry-2026-10-10.md`, and the dated October 10-11 entries
in my lab notes.

Live continuation check (October 11): expected test-card CID confirmed over
Wi-Fi SSH; Debian reports 12.15, kernel reports `7.2.0-rc5-nzeus-gma-native-host+`
build #9, MemTotal 117568 KiB. Both `apt-get -s check` and
`apt-cache depends python3` reproduced the abort (exit 134). No reboot or
card installation was needed for these checks.

Continuation progress: `f467e58` fixes ALSA partial-write handling and retains
pending PCM frames. Stock MIPS build is 2677 KiB under the 3072 KiB limit;
modern MIPS, fake ALSA/hardware checks and the 224-picture QEMU self-test pass.
Modern helper tests cover 13 cases, including concurrent commands and restart
rollback. The corrected rootfs PC suite passes 268 checks and its fake-card
recovery suite passes 70/70. The reviewed car GUI passes 603 checks (`ade8b94`).
The app/helper changes were installed on BLACK on October 12. See
`../reports/continuation/README.md` for the validation and bundle state.
The October 11 final SSH timeout was superseded by successful October 12 USB
SSH checks, preflight and application installation. This does not deploy the
separately built newer kernel or produce a fresh complete Debian image.

Native Python 3.11.2 passed a real pseudo-terminal and certificate-verified
HTTPS fetch; curl also returned HTTP 200. These checks ran on the console,
not QEMU. Apt remains broken: the diagnostic can bypass the abort by copying
one float constant through integer registers, but the unchanged library still
fails `apt-get -s check`. See `diagnostics/apt/README.md` for the controlled
crossover and remaining uncertainty.
October 12 BLACK check: initial check/depends passed, but a package-install
simulation and two subsequent checks aborted with the same exception using
the unchanged libapt hash. No packages were installed. The problem occurs on
both physical units and remains unresolved; one passing run is insufficient.
Further BLACK diagnostics: the 1,323,000-sample Python arithmetic probe and
two-process FP-register context tests (FP32 and FPXX, 10,000 rounds each)
passed. Moving apt's transfer to FP register 2 and disabling ASLR per-process
both still permitted the crash; unchanged checks also passed on later runs.
The fault is intermittent, and neither test establishes a general FPU defect
or a supported fix. See `diagnostics/apt/README.md`. The installed library and
working GUI remain unchanged.
Transfer-recording copies subsequently passed 7 native checks, recording 478
initializations per run with no mismatch; they did not capture the failure.
Three fresh uninstrumented checks still aborted. An exact-build-#9 `nofpu`
boot candidate was installed on the shared SD on October 12 after a fresh
reader identity check and stock-extent snapshot. All 109 changed blocks passed
preimage checks and readback; exact snapshot recovery is retained. Only the
boot-argument text differs in the decompressed image. Native boot confirms
nofpu, unchanged app/libapt hashes and 117568 KiB RAM; USB serial/SSH and native
Python arithmetic smoke operations work. All 20 basic apt checks and 10 package
install simulations passed without overrides. Nothing was installed. The
baseline crossover and my visual/control check on this boot remain pending.
This supports investigating the hardware-FPU execution path, not a proven fix
or silicon diagnosis. See `diagnostics/apt/README.md` for evidence/test criteria.
Root/data areas, installed app/libapt and NOR were not written by the card test.
The exact original build #9 kernel was subsequently restored through a NEW
reader snapshot and separate recovery plan. All 109 fresh preimages and all
109 written blocks passed verification. The card now contains the hardware-FPU
baseline (`5aa7e571...`); its return boot and apt crossover are completed. Earlier
nofpu test logs remain valid, but nofpu is no longer the kernel on this card.
Home screen confirmed on the console. Live CPU capabilities restore fpu, nofpu is absent,
and original app/libapt/kernel hashes match. The same 30-command runner passes
8/20 checks and 1/10 install simulations; 21 abort with the original exception.
The nofpu run passed 30/30. A new FP32/FPXX first-touch memory-page probe passes
all 32,768 native transfers with/without integer prefault, failing to reproduce
apt's error. Staged RAM files were verified and removed. The hardware-FPU path
is strongly implicated, but root cause and a maintainable fix remain open.

## Built, installed and unfinished are different states

October 12 further apt isolation: the integer-store diagnostic passes 9/9
native checks while its matched FP-store control fails 7/9. Integer-load-only
still fails 3/3. A separate signal test recorded the same bit loss once, but
eight repeats and 80,000 forced signal deliveries did not reproduce it. This
narrows the store-path/timing investigation; it is not an installed fix.
Original kernel/app/libapt and `/data` are preserved. See the apt README.

- Private GitHub baseline at continuation start: `f080872`. Its USB serial
  driver additions and newer ALSA underrun fix were built on the PC, but the
  saved card installation evidence precedes those additions.
- The installed ALSA package recorded in the last verified card update has
  SHA-256 prefix `5aa7`. Do not assume the current build directory matches it.
- (Resolved 2026-10-12) The Debian image manifest now records commit `ac6300c`
  and a full provenance JSON; see the dated note at the top.
- Claude's interrupted car/app/helper changes remain in the local worktree
  at continuation start. Their presence is not evidence of review, passing
  tests, installation, or a GitHub backup. A local source checkpoint was saved
  before further edits; raw card images and private configuration stay out of git.

**Deck installed on BLACK (2026-10-12 15:57, direct copy, not the signed updater):**
`/usr/local/bin/nzeus-modern` SHA-256 `95162cfe9d493a50...` (commits 58ed048 GM read-only
GMLAN discovery, 4490dcd fast-screen code, 694e20d desktop mode); previous app kept as
`/usr/local/bin/nzeus-modern.prev-desktop` (`b5e91a0d...`). The signed updater's manifest
still lists the old hash. Desktop render checked on the console. Fast screen is OFF (needs
host kernel 68e9f526 + the flag NZ_DIR/fast-screen); kernel not installed yet.

## Open work, in order

Current daily setting is **900 MHz with hardware FPU**, with the October 13
30/30 apt result described in my lab notes. The older nofpu fallback and
1188 MHz investigation paragraphs are historical, not the current work plan.

1. Repeat the real 2016 Chevrolet/Silverado scan after the installed connection
   recovery fix. Verify live readings resume without Connect again. Export the
   same saved scan in English and Arabic, then open both through Files. The
   first EX field scans, report generation and PDF reading are verified; the
   exact original post-scan failure trigger remains unknown.
2. Complete actual vehicle-feature coding and a hosted vehicle support catalog.
   Requested Ranger 2020-2026, Evoque 2020-2026 and Silverado 2010-2026 ranges
   are targets, not validated coding coverage. Signed guide transport/cache and
   read-only discovery are installed. A catalog host, verified module profiles,
   a reviewed write controller and tested configuration backup/restore are
   still needed. Discovery files are not configuration backups.
3. Complete live app integration checks. WhatsApp's corrected key now achieves
   repeated successful signed logins from my hotspot; the PC bridge
   reports ready. I confirm chats appear and received photos/voice
   notes work; home LAN authentication also passes. Arabic/emoji and sending
   still need explicit field verification.
   Claude's real account/session and recovery behavior need verification beyond
   the passing local SSH/PTY regression. Modern Claude does not currently create
   the older reverse-access tunnel. See the October 13 WhatsApp setup handoff.
4. Validate battery calibration, repeated cold boots, Wi-Fi/USB-role switching
   and sustained loaded use. Headphone playback and the manual speaker switch
   were already tested October 12; automatic plug sensing is a known limitation,
   not an outstanding routine test.
   The white USB-C charging incident on GREEN is not diagnosed. Continue using
   the previously working USB-A charging connection for normal work.
5. Fresh-card Debian root component: refreshed and PC-tested October 14
   (`9824ce92...`, see top); not a complete SD disk image or universal installer,
   and not yet booted on a fresh physical card. Finish provisioning/installation
   for supported fresh cards and hardware-test it. Routine updates on the existing
   card preserve /data and use the verified managed-file path. Validate desired
   compiler/git/tmux workloads and supported additional packages on hardware.

Minor housekeeping: two archived helper-copy directories remain after the
documented native-removal/automatic-approval block. The fresh real-card baseline,
old-file history purge and obsolete Web screenshot removal are already complete.

Bluetooth audio/controllers, HDMI, USB audio and webcams are not established
features. USB audio is disabled in the current host build. Native TCP/IP alone
does not prove that every Linux application is available on 32-bit MIPS.
October 14 HDMI investigation traces the vendor H1600 PHY startup in three
synthetic PC cases; no hardware accesses or driver installation occurred.
See `purelinux/HDMI-PC-DECODE-2026-10-14.md`. PHY identity, monitor/EDID,
output routing and a recovery-backed diagnostic remain to be established.

## Recovery constraints

Keep the case closed for software work. No NOR writes. Card changes require
fresh identity/snapshot, verified sector deltas and matching recovery profiles.
Never write raw devices from SSH on the running console. Preserve `/data`,
original `Bubbles.scr` and stock `dtb.bin`. Do not publish card images, private
keys, Wi-Fi configurations or account/session material to the repository.
