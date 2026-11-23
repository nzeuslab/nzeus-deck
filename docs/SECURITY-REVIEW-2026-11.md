# NZEUS Deck security review, November 2026

*Naser Ashknani, NZEUS LAB. Reviewed: NZEUS Deck at commit `43ce9f3` (v1.0-beta.2).*

*I chose what to review and decided what to fix. Claude Code did most of the
code reading and wrote the fixes and the tests.*

I reviewed NZEUS Deck's network-facing code and the code that handles
untrusted files, looking for ways another person could take over a console
or read its owner's data. Each problem below was **confirmed by a test that
fails on the old code** and passes on the fixed code. The tests are in this
repository, so anyone can re-run them.

## What was in scope

**Attackers considered:**
- someone near the console who controls Wi-Fi network names or runs a fake access point
- someone on the same network (home or public Wi-Fi), including a man-in-the-middle
- hostile content arriving as a picture, a message or a web page

**Out of scope (by design, see `SECURITY.md`):** anyone holding the console,
its SD card or a USB cable has full control.

**Reviewed:** SSH server and its settings, Wi-Fi scanning and joining, the
over-the-air update system (console and PC side), the WhatsApp and Claude
connections, key storage, the web browser launcher, `nzeus-fetch`, the
picture decoder, and the Arduino upload command.

## Summary

| ID | Problem | Severity | Status |
|---|---|---|---|
| NZD-01 | BMP picture decoder reads memory outside the file | Medium | Fixed |
| NZD-02 | An old signed update can be replayed (downgrade) | Low | Fixed |
| NZD-03 | A nearby access point can hide other networks from the Wi-Fi list | Low | Fixed |
| NZD-04 | A file name with `'` runs a command in the Arduino upload | Low | Fixed |
| NZD-05 | First connection to the home PC trusts any SSH host key | Low | Open, workaround below |
| NZD-06 | Smaller issues (list below) | Info | Documented |

Severity is my own judgement, based on who can reach the problem and what
they get.

## NZD-01: BMP decoder reads outside the file (Medium, fixed)

**Where:** `apps/nzeus-gui/picture.c`, `load_bmp()`. NZEUS Deck's own code,
not a third-party library. It is used by the Images app, wallpapers, custom
icons and WhatsApp photos.

**What was wrong:** three numbers from the file's header were trusted without
checking them against the file's real size:
1. the header size, which says where the colour table starts;
2. the pixel start, where `start + rows` could wrap round to a small number
   on the console's 32-bit processor and pass the size check;
3. the number of colours in the table.

A crafted BMP file could make the decoder read memory before or after the
file.

**Impact:** the decoder only *reads*, so this is not a way to run code. A
hostile picture can crash NZEUS Deck (which runs as root) or turn stray
memory into the pixels shown on screen. To reach it, the owner has to open
the file, or it has to arrive as a WhatsApp photo. That last path depends on
the PC-side bridge, which is not in this repository.

**Fix:** every header value is checked against the file size, the size maths
is done in 64 bits, and a colour number the table doesn't have is drawn
black.

**Proof:** `build_picture_test.sh` builds `apps/nzeus-gui/tests/test_picture_bmp.c`
with AddressSanitizer, a compiler option that stops a program at its first
read outside valid memory. Five cases: one normal BMP and four hostile ones.
Results:

| Case | Old code, 32-bit MIPS (qemu) | Fixed code |
|---|---|---|
| 1. normal 8-bit BMP | decodes correctly | decodes correctly |
| 2. header size past the end of the file | **crash** (segmentation fault) | refused |
| 3. pixel start that wraps round | **accepted** | refused |
| 4. colour table longer than the file | **accepted** | refused |
| 5. colour number past the table | drawn black | drawn black |

On the PC (64-bit) AddressSanitizer also stops the old code at case 2:
`READ memory access ... in load_bmp picture.c:134`. Case 3 only goes wrong
on 32-bit systems such as the console, which is why it was also run as a
MIPS program under qemu.

## NZD-02: Old signed update can be replayed (Low, fixed)

**Where:** `linux-port/deck-modern/nzeus-update.sh`.

**What was wrong:** the update system itself is sound. The file list is
signed (HMAC-SHA256 with a key shared only by the owner's PC and the
console), every file is checked by SHA-256, and the signature compare is
constant-time. But nothing stopped an *older* signed list from being
installed. Someone on the same network who had saved an earlier update could
serve it again and roll the console back to old software.

**Impact:** only the owner's own older builds can be installed this way.
That matters only if an older build has a known problem (for example, one
without the NZD-01 fix).

**Fix:** the console refuses a signed list whose version is older than the
installed one. The owner can still go back on purpose with
`NZ_UPD_ALLOW_OLDER=1 nzeus-update.sh install` over SSH.

**Proof:** two new tests in `linux-port/deck-modern/tests/test_helpers.py`.
`test_older_signed_update_is_refused` failed on the old script ("check"
accepted the older list) and passes now. All 20 update tests pass.

## NZD-03: Nearby access point can hide networks (Low, fixed)

**Where:** `linux-port/deck-modern/wifi-scan.sh`.

**What was wrong:** the scan result went through `echo`. Debian's `/bin/sh`
(dash) treats backslash codes in `echo`'s text as commands: `\c` means
"stop printing here". Network names are escaped by `iw`, but some text that
access points send about themselves (WPS fields such as "Device name") is
printed as it is. An access point named with `\c` in that field could hide
every network listed after it.

**Fix:** `printf '%s\n'` instead of `echo`.

**Proof:** a fake scan result with three networks, the first one using
`Device name: AP\cHIDE`. Run under dash, the old script listed **1**
network and the fixed script lists all **3**. Not yet tried with a real
access point sending that name.

## NZD-04: File name runs a command in the Arduino upload (Low, fixed)

**Where:** `apps/nzeus-gui/app_arduino.c`, `upload_start()`.

**What was wrong:** the chosen firmware file's path was put inside `'...'`
in a shell command. A `'` in the file name would end the quoting, and the
rest of the name would run as a command (as root).

**Impact:** the files come only from the owner's own SD card folders, and no
network path writes file names there. So this needs someone who can already
put files on the card.

**Fix:** names containing `'` are no longer offered for upload.

## NZD-05: First SSH connection to the home PC trusts any key (Low, open)

**Where:** `nzeus-wa.sh` and `nzeus-claude-connect.sh` connect to the
owner's PC with `dbclient -y`: an unknown host key is accepted the first
time and refused if it ever changes ("trust on first use").

**Impact:** if the *first* connection to the PC's public address happens on
a hostile network, a man-in-the-middle could pose as the PC from then on. It
would see the messages and Claude sessions sent through it and could show
fake ones. It cannot log in to the real PC, because SSH key logins can't be
passed on.

**Workaround now:** make the first connection to each address (home and
public) on your own home network.

**Planned fix:** save the PC's host key during setup at home, then drop `-y`.

## NZD-06: Smaller issues (Info)

- **SSH listens on Wi-Fi too.** That's intended, as a second way in if the
  screen fails. Logins are key-only and SSH only starts when a key is
  installed. The settings file comment wrongly said "USB cable only"; it is
  corrected.
- **Lock icon can be faked.** A network named like `x[WPA2]` is shown as
  secured even when it is open, because the flag search also looks at the
  name. This only affects the list, not the connection.
- **Wi-Fi password visible while joining.** During a join the password is on
  a command line, which local processes can read. There are no other users
  on the console.
- **Fixed `/tmp` file names.** Some status files have fixed `/tmp` names and
  are written as root. There are no other users or services that could
  abuse this.
- **Two browser start-page links use plain HTTP** (frogfind, 68k.news), so a
  man-in-the-middle can change those pages.
- **`nzeus-fetch` follows HTTPS-to-HTTP redirects.** Certificates are always
  checked for HTTPS.

## Checked and found fine

- **SSH:** keys only (no passwords compiled in), no port forwarding, and
  ed25519/ECDSA keys only. Every console makes its own host key on first
  start; it is not shared by all images.
- **Wi-Fi joining:** network names are stored as hex, the key is derived by
  `wpa_passphrase` from a password passed on stdin (newlines refused), and
  shell quoting in `wifi.c` is safe for any text.
- **Updates:** signature, SHA-256 per file, a fixed list of allowed
  destinations (no `../`), MIPS-only programs, all files checked before any
  is changed, automatic rollback if the new version doesn't keep running.
- **WhatsApp and Claude apps:**
  - every ID and file name is checked before it reaches a command;
  - message text is sent as base64;
  - buffers are bounded;
  - keys are kept locked (PBKDF2 + ChaCha20 + HMAC) and unlocked only into
    private RAM files.
- **`nzeus-fetch`:** certificate checks are never turned off, and only
  http/https are allowed.
- **DHCP:** only address values are used.

## Not reviewed

The PC-side WhatsApp bridge (not in this repository), the Arabic text
shaping code, and the third-party PNG/JPG decoders that come with LVGL.

## Re-running the checks

```
wsl -d Ubuntu -- bash build_picture_test.sh                     # NZD-01
wsl -d Ubuntu -- python3 linux-port/deck-modern/tests/test_helpers.py   # NZD-02 + update tests
```

## Reporting

Found something else? Please report it privately: see `SECURITY.md`.
