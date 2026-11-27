# Dropbear SSH for the modern-Linux console

`dropbearmulti-modern-mipsel`: Dropbear **2026.94** with `localoptions.h`
(keys only, no passwords) and `nss_fallback.c` from this folder, static, stripped,
1,394,916 bytes, MIPS32r2 little-endian, FP ABI "any FPU" (FPXX). Built with
Ubuntu's `mipsel-linux-gnu-gcc` 12.4 + glibc 2.39, `-march=mips32r2 -mtune=74kc -O2`,
stack protector + FORTIFY, non-executable stack. sha256 in `SHA256SUMS`,
details in `build-info.txt`. Source: the `DROPBEAR_2026.94` tag of the author's
public mirror (mkj/dropbear; the official tarball site was blocked from the build
machine). Rebuild: `bash ports/r36s-hichip/usbnet/dropbear/build-dropbear-modern.sh [tarball or folder]`
(with no argument it downloads the release tarball once to `~/r36s/downloads/`;
every tarball is checked against the pinned SHA-256 in the script,
`e098034a...` - the cached copy on the PC matched it on 2026-10-13).
Licence: `LICENSE` (MIT-style, from the source).

**Not yet run on the console.** Tested on the PC under qemu-mipsel only.

## The crash of the stock binary (what we know)

On the modern kernel the stock `apps/dropbear/mips/dropbearmulti` (HiChip SDK
toolchain, glibc 2.20, `-mfp32`, two NZEUS patches) died at once:

    invalid write access to 0048a0e8, epc 0048a0f0 in dropbearmulti[8a0f0,400000+f0000]

Read plainly: the instruction at 0x48a0f0 tried to **store** to 0x48a0e8 - 8 bytes
before itself - and both lie in the program's **first load segment** (0x400000,
0xf0000 bytes = code + read-only data), which Linux maps read+execute, not
writable. So the program wrote into its own code area. Normal compiled C never
does that; candidates:

1. the old SDK linker put writable data (or a relocation/trampoline area) into the
   read+execute segment, or marked that segment RWX and the new kernel maps it
   differently;
2. startup code of the old static glibc 2.20 takes a different path on the new
   kernel (it sees a different auxiliary vector, FP mode, TLS/`rdhwr` emulation,
   page layout...) and that path writes where it should not;
3. a self-modifying stub (e.g. a packer) - unlikely, the build script does not pack.

Notable: per `apps/dropbear/README.md` the stock MIPS binary passed 15/15 under
qemu-mipsel on the PC, and qemu-user does enforce read-only code pages - so, if
the card copy is that same file, the bad write only happens with the console's
kernel (2 fits that best). **The exact cause is NOT determined**:
the stock binary is not in the repo or the cloud machine. One read-only PC command
settles it:

    bash ports/r36s-hichip/usbnet/dropbear/explain-stock-crash.sh apps/dropbear/mips/dropbearmulti

(program headers, which section holds 0x48a0e8/0x48a0f0, the function and the code
around it).

The new build avoids every candidate we can control: different toolchain and
glibc, two separate load segments (code `R E`, data `RW`, checked by the build
script), no RWX, no packing, FPXX. Whether it runs on the console is the next
hardware test - qemu passing proves the program, not the console kernel.

## Files

| File | What |
|---|---|
| `dropbearmulti-modern-mipsel` | the program (server `dropbear`, `dbclient`, `dropbearkey`, `dropbearconvert`, `scp`) |
| `build-dropbear-modern.sh` | rebuilds it (WSL Ubuntu: `gcc-mipsel-linux-gnu`, `qemu-user`) |
| `dropbear-start.sh` | console: start/stop the server on 192.168.77.1:22 only |
| `test-dropbear-pc.sh` | PC test: MIPS server+client key login under qemu; start script with the boot BusyBox (21/21) |
| `explain-stock-crash.sh` | read-only look at the stock binary's crash address |

## Using it (after `../usbnet-up.sh` + confirm + `pc-setup.ps1` ping OK)

1. A key on the PC (once): `ssh-keygen -t ed25519` (Windows has OpenSSH built in).
   Only ed25519/ecdsa keys work (RSA is not built in). **Never commit private keys.**
2. Upload to RAM over the rescue serial port (full readback check each time):
   ```
   python ports\r36s-hichip\diagnostics\rescue-upload.py ports\r36s-hichip\usbnet\dropbear\dropbearmulti-modern-mipsel --name dropbearmulti-modern
   python ports\r36s-hichip\diagnostics\rescue-upload.py %USERPROFILE%\.ssh\id_ed25519.pub --name authorized_keys
   ```
3. Start: `python ports\r36s-hichip\diagnostics\rescue-command.py ports\r36s-hichip\usbnet\dropbear\dropbear-start.sh`
   It prints the host key **fingerprint** over the serial cable (trusted path).
4. Log in: `ssh -o UserKnownHostsFile=%USERPROFILE%\.ssh\nzeus_known_hosts root@192.168.77.1`
   and compare the fingerprint ssh shows with step 3. Copy files with `scp -O`
   (new OpenSSH scp uses SFTP, which is not built in).
5. Stop: `sh /tmp/nzeus-usbnet/dropbear-start.sh --stop` (from any console shell).

Host key: by default a NEW key is made in RAM at every start after a reboot, so
ssh warns "host key changed" after each reboot (that is why a separate
`nzeus_known_hosts` file is suggested: delete its line after a reboot). To keep
one identity, make a key once on the PC and keep it OUTSIDE the repo, e.g.
`qemu-mipsel ports/r36s-hichip/usbnet/dropbear/dropbearmulti-modern-mipsel dropbearkey -t ed25519 -f ~/r36s/keys/nzeus-modern-host`,
then upload it each boot with `--name ssh_host_ed25519`.

What the start script sets up (RAM only): `/tmp/nzeus-ssh` (mode 700) as root's
home with `.ssh/authorized_keys` - Dropbear checks the folders from the key file
up to the home folder, so the world-writable `/tmp` is never checked;
`/etc/passwd` with one root line (the RAM system has no `/etc`; the static
program reads it via `apps/dropbear/nss_fallback.c`); `scp` on PATH for logins.
Server options: `-F -E -e -j -k` (foreground under `setsid`, log to
`/tmp/nzeus-ssh/dropbear.log`, pass PATH, **no port forwarding**), listen only on
`192.168.77.1:22`, refuses to start unless `usbnet-up.sh` set that address.
