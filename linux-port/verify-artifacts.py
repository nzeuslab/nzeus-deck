"""Verify PC-built kernel packaging, not hardware operation. Run after the build."""
import gzip
import hashlib
import struct
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
out = root / "artifacts"

for line in (out / "SHA256SUMS").read_text().splitlines():
    expected, name = line.split(maxsplit=1)
    assert hashlib.sha256((out / name).read_bytes()).hexdigest() == expected, name

kernel = (out / "vmlinuz.bin").read_bytes()
dtb = (out / "r36s-e3100-bringup.dtb").read_bytes()
image = (out / "nzeus-r36s-experimental-with-dtb.bin").read_bytes()
assert image == kernel + dtb, "Appended board tree does not match"
assert struct.unpack_from(">I", dtb)[0] == 0xD00DFEED
assert struct.unpack_from(">I", dtb, 4)[0] == len(dtb)

def read_elf(path):
    """Read the load segments and named sections from a 32-bit MIPS ELF."""
    data = path.read_bytes()
    assert data[:7] == b"\x7fELF\x01\x01\x01", path
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    assert header[1] == 8, "Expected MIPS"
    entry, phoff, shoff = header[3:6]
    phsize, phnum, shsize, shnum, names_index = header[8:13]
    segments = [struct.unpack_from("<8I", data, phoff + i * phsize)
                for i in range(phnum)]
    loads = [segment for segment in segments if segment[0] == 1]
    sections = [struct.unpack_from("<10I", data, shoff + i * shsize)
                for i in range(shnum)]
    strings = sections[names_index]
    names = data[strings[4]:strings[4] + strings[5]]
    named = {names[s[0]:].split(b"\0", 1)[0].decode(): s for s in sections}
    return data, entry, loads, named


# Verify the append point against the linked kernel, not just concatenation.
compressed, entry, loads, sections = read_elf(out / "nzeus-compressed.elf")
assert len(loads) == 1, "Review packaging for multiple load segments"
_, offset, address, physical, file_size, memory_size, _, _ = loads[0]
assert entry == address == physical == 0x81000000, "Review loader entry address"
assert kernel == compressed[offset:offset + file_size], "Raw image differs from ELF"
tree_section = sections[".appended_dtb"]
assert tree_section[1] == 8, "Expected reserved NOBITS section"
tree_offset = tree_section[3] - address
assert tree_offset == len(kernel), "DTB is not at the kernel's expected address"
assert len(dtb) <= tree_section[5], "DTB exceeds its reserved space"
assert image[tree_offset:tree_offset + 4] == b"\xd0\x0d\xfe\xed"
ram_end = 0x02F91E50
assert (physical & 0x1FFFFFFF) + memory_size <= ram_end, "Decompressor exceeds RAM"
_, _, plain_loads, _ = read_elf(out / "nzeus-kernel.elf")
for segment in plain_loads:
    # Reserve an extra megabyte for the copied board tree after decompression.
    assert (segment[3] & 0x1FFFFFFF) + segment[5] + 0x100000 <= (physical & 0x1FFFFFFF), \
        "Uncompressed kernel may overlap its decompressor"
print(f"PASS: ELF entry {entry:#x}, DTB offset {tree_offset:#x}, load regions within RAM")

config = (out / "kernel.config").read_text()
for flag in ("INET", "UNIX98_PTYS", "BLK_DEV_INITRD", "DEVTMPFS", "TMPFS",
             "MIPS_RAW_APPENDED_DTB", "USB_CONFIGFS_ECM", "USB_CONFIGFS_ACM"):
    assert f"CONFIG_{flag}=y\n" in config, flag
for flag in ("CEVT_R4K", "CSRC_R4K"):
    assert f"CONFIG_{flag}=y\n" in config, flag
assert "CONFIG_MIPS_EXTERNAL_TIMER=y" not in config
assert "CONFIG_HC16XX_TIMER=y" not in config
for node in ("/timer@4", "/dw-mmc@1884c000", "/gpio-controller@18800044", "/pwm@18818410"):
    assert subprocess.check_output(["fdtget", "-t", "s", str(out / "r36s-e3100-bringup.dtb"),
                                    node, "status"], text=True).strip() == "disabled", node

# Check every initramfs entry instead of only trusting a printed file listing.
archive = gzip.decompress((out / "initramfs.cpio.gz").read_bytes())
entries = {}
pos = 0
while True:
    assert archive[pos:pos + 6] in (b"070701", b"070702"), "Bad newc archive"
    fields = [int(archive[pos + 6 + 8*i:pos + 14 + 8*i], 16) for i in range(13)]
    size, namesize = fields[6], fields[11]
    name = archive[pos + 110:pos + 110 + namesize - 1].decode()
    pos = (pos + 110 + namesize + 3) & ~3
    payload = archive[pos:pos + size]
    pos = (pos + size + 3) & ~3
    if name == "TRAILER!!!":
        break
    entries[name.lstrip("/")] = payload
assert entries["init"] == (root / "init").read_bytes()
assert entries["usb-rescue"] == (root / "usb-rescue").read_bytes()
assert entries["bin/busybox"] == (out / "busybox-mipsel").read_bytes()
# gen_init_cpio includes the terminating NUL in symlink payloads.
assert entries["bin/sh"].rstrip(b"\0") == b"busybox"
for applet in ("ash", "mount", "mkdir", "setsid", "cttyhack", "sleep", "ip", "free", "ln", "stty"):
    assert f"bin/{applet}" in entries, applet

busybox = out / "busybox-mipsel"
elf = busybox.read_bytes()
assert elf[:6] == b"\x7fELF\x01\x01", "Need 32-bit little-endian ELF"
assert struct.unpack_from("<H", elf, 18)[0] == 8, "Need MIPS machine type"
subprocess.run(["qemu-mipsel", str(busybox), "ash", "-n", str(root / "init")], check=True)
subprocess.run(["qemu-mipsel", str(busybox), "ash", "-n", str(root / "usb-rescue")], check=True)
# Mock the stock kernel name and mount, so even a broken guard cannot mount anything.
guard = subprocess.run(["qemu-mipsel", str(busybox), "ash", "-c",
    'uname() { echo 4.4.186-release; }; mount() { exit 99; }; . "$1"',
    "guard-check", str(root / "usb-rescue")], capture_output=True, text=True)
assert guard.returncode == 1 and "Refusing USB setup" in guard.stderr, guard
print("PASS: USB rescue refuses the stock kernel before mounting configfs")
subprocess.run(["qemu-mipsel", str(busybox), "ash", "-c",
                'test "$((7 * 6))" = 42 && echo "PASS: MIPS shell execution"'], check=True)
ram = subprocess.check_output(["fdtget", "-t", "x", str(out / "r36s-e3100-bringup.dtb"),
                               "/memory", "reg"], text=True).strip()
assert ram == "0 2f91e50", ram
bootargs = subprocess.check_output(["fdtget", "-t", "s", str(out / "r36s-e3100-bringup.dtb"),
                                    "/chosen", "bootargs"], text=True).split()
assert "clk_ignore_unused" in bootargs, "Preserve firmware clocks during initial SD bring-up"
print(f"PASS: hashes, kernel + DTB packaging ({len(image):,} bytes), native IPv4/PTYS config")
print(f"PASS: initramfs contains exact startup script and MIPS shell ({len(entries)} entries)")
print("PASS: compiled board retains the measured stock Linux memory range")
print("HARDWARE: not booted; no display, RAM reclaim, Wi-Fi or standalone SD boot verified")
