# nzeus-bt: NZEUS Deck's Bluetooth program (SD Debian)

`nzeus-bt` finds, pairs and connects Bluetooth keyboards and mice (classic
HID and Bluetooth Low Energy) through the kernel's management socket,
and keeps the pairings in the Deck's folder on `/data`
(`/data/nzeus/deck/nzeus/bluetooth`). The Deck runs it through the small
wrapper `../nzeus-bt` (installed as `deck-bin/nzeus-bt`, the program itself as
`deck-bin/nzeus-bt.bin`); `../nzeus-bt-up.sh` is "Bluetooth on".

| What | Where |
| --- | --- |
| Source | `src/` (C, no libraries beyond libc) |
| Console program | `nzeus-bt-mipsel` (static MIPS32r2, stripped), hash in `SHA256SUMS` |
| PC tests | `tests/` (unit tests + every command against `fake_kernel.py`; `mutants.sh` = deliberate mistakes the tests must catch) |

Build and test (WSL, from the R36S folder; uses the HiChip SDK's MIPS compiler
in `~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot`):

    bash linux-port/deck-modern/bt/build.sh

It writes `nzeus-bt-mipsel` + `SHA256SUMS` only after all tests pass.
`NZBT_OUT=FILE` writes a check build elsewhere. `NZBT_MUTANTS=1` adds the C
mutants (about 40 minutes).

The system image (`linux-port/rootfs/debian/apply-overlay.sh`) and the updater
(`tools/publish_update.py`) both refuse a `nzeus-bt-mipsel` that does not match
`SHA256SUMS`.

Moved here from `apps/bluetooth/src` on 2026-10-13. The program was rebuilt
from the moved source and came out byte-identical to the one running on the
console (`dd7d5250...`, `deck-bin/nzeus-bt.bin` in the 2026-10-12 live hashes);
unit tests 230/230 (x86 + MIPS), flow tests 112/112 (x86 + MIPS under qemu).
PC tests prove the PC side only; Bluetooth itself was proven on the console
(BLE mouse pairing, 2026-10-11).

## nzeus-bt commands (for people and for the Settings app)

Exit code 0 = success, 1 = failed, 2 = wrong usage. Problems are printed as a
line starting with `error ` (stop) or `warning ` (worked, but read this).

| Command | Output |
|---|---|
| `nzeus-bt on` | `on hci0 ADDRESS, N paired devices` |
| `nzeus-bt off` | `off` |
| `nzeus-bt status` | `key=value` lines: `loaded=yes/no`, `controller=hci0/none`, `address=`, `name=`, `powered=yes/no`, `le=yes/no`, `paired=N`, `connected=N`, `service=running/stopped` |
| `nzeus-bt scan [SECONDS]` | after SECONDS (default 10, 1-120): one line per device, strongest first:<br>`ADDRESS<TAB>NAME<TAB>KIND<TAB>TYPE<TAB>RSSI<TAB>SUPPORTED`<br>KIND: keyboard, mouse, keyboard+mouse, gamepad, joystick, remote, tablet, input, phone, computer, audio, watch, other...<br>TYPE: classic, le, dual. RSSI: signal in dBm (-40 strong, -90 weak; 127 or 0 = unknown). SUPPORTED: yes/no. Name `(no name)` if unknown. |
| `nzeus-bt pair ADDR [--pin NNNN]` | progress lines (below), ends with `connected` (classic) or `connecting - ...` (LE). Up to 90 s. |
| `nzeus-bt connect ADDR` | `connected` / `connected (already)` / `connecting - ...` (LE) |
| `nzeus-bt disconnect ADDR` | `disconnected` (stays paired) |
| `nzeus-bt list` | paired devices: `ADDRESS<TAB>NAME<TAB>KIND<TAB>TYPE<TAB>connected/not-connected` (TYPE classic/le) |
| `nzeus-bt remove ADDR` | `removed` (forgets the key) |
| `nzeus-bt daemon` | the background service (started by ../nzeus-bt-up.sh) |

`pair` progress lines (one per line, first word = what happened):

| Line | Meaning / what the app shows |
|---|---|
| `pairing ADDR (classic)...` | started |
| `passkey 123456 - type it on the keyboard, then press Enter` | show the 6 digits big: "Type 123456 on the keyboard, then press Enter" |
| `typed 3` | the keyboard reports 3 digits typed so far (optional dots) |
| `confirm 123456 - accepted ...` / `confirm - accepting ...` | info only (accepted automatically) |
| `enter-passkey - type the number the device shows, then Enter` | the DEVICE shows a number: ask for it with a number pad and write it + newline to nzeus-bt's stdin |
| `pin 482913 - type it on the keyboard, then press Enter` | old keyboard: show the PIN like a passkey |
| `pin 0000 - using the usual PIN ...` | old mouse: info only |
| `paired` | keys saved |
| `connected` / `connecting - ...` | done |
| `error ...` | failed: show the text |
| `warning ...` | worked, show the text small |
