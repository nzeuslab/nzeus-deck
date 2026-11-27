"""PC-only SD packaging experiment. Never installs, boots, or writes a device.

Run verify-artifacts.py in WSL first. This wraps the self-decompressing modern
image in the factory-tested gzip uImage + ChaCha20 format. Packaging success is
not a claim that the factory-to-modern hardware handoff is ready.
"""
import gzip
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root / 'boot-analysis'))
from image_codec import check_uimage, decode, encode, factory_key, test_rfc_block

artifacts = root / 'artifacts'
for line in (artifacts / 'SHA256SUMS').read_text().splitlines():
    expected, name = line.split(maxsplit=1)
    if hashlib.sha256((artifacts / name).read_bytes()).hexdigest() != expected:
        raise ValueError('Artifact hash mismatch: ' + name)

raw = (artifacts / 'nzeus-r36s-experimental-with-dtb.bin').read_bytes()
kernel = (artifacts / 'vmlinuz.bin').read_bytes()
dtb = (artifacts / 'r36s-e3100-bringup.dtb').read_bytes()
assert raw == kernel + dtb
assert b'clk_ignore_unused' in dtb
assert struct.unpack_from('>I', dtb)[0] == 0xd00dfeed
assert struct.unpack_from('>I', dtb, 4)[0] == len(dtb)

elf = (artifacts / 'nzeus-compressed.elf').read_bytes()
assert elf[:7] == b'\x7fELF\x01\x01\x01'
h = struct.unpack_from('<HHIIIIIHHHHHH', elf, 16)
assert h[1] == 8 and h[3] == 0x81000000
loads = []
for i in range(h[9]):
    segment = struct.unpack_from('<8I', elf, h[4] + i * h[8])
    if segment[0] == 1:
        loads.append(segment)
assert len(loads) == 1
_, offset, address, physical, size, memsize, _, _ = loads[0]
assert address == physical == h[3]
assert kernel == elf[offset:offset + size]
# Do not overwrite the still-running factory bootloader while unpacking.
# 0x01da0000 is the SD tree's mirrored bootmem start, not extra Linux RAM.
assert (physical & 0x1fffffff) + max(memsize, len(raw)) <= 0x01da0000

payload = gzip.compress(raw, compresslevel=9, mtime=0)
# Use the same image OS/architecture/type/gzip combination as the proven probe.
header = bytearray(struct.pack('>7I4B32s', 0x27051956, 0, 0, len(payload),
                              address, h[3], zlib.crc32(payload),
                              5, 5, 2, 1, b'NZEUS modern SD experiment'))
struct.pack_into('>I', header, 4, zlib.crc32(header))
plain = bytes(header) + payload
info = check_uimage(plain)
key = factory_key((root / 'boot-analysis/factory-bootloader.bin').read_bytes())
test_rfc_block()
nonce = hashlib.sha256(b'NZEUS-MODERN-SD-v1' + plain).digest()[:12]
wrapped = encode(plain, key, nonce)
decoded = decode(wrapped, key)
assert decoded == plain
assert gzip.decompress(decoded[64:]) == raw
check_uimage(decoded)

out = artifacts / 'sd-candidate'
out.mkdir(exist_ok=True)
name = 'modern-kernel.uImage.enc.PC-ONLY'
(out / name).write_bytes(wrapped)
report = {
    'status': 'PC_PACKAGED_HARDWARE_UNTESTED',
    'image': info,
    'bytes': len(wrapped),
    'sha256': hashlib.sha256(wrapped).hexdigest(),
    'raw_image_sha256': hashlib.sha256(raw).hexdigest(),
    'decompressor_physical_end': hex((physical & 0x1fffffff) + memsize),
    'factory_bootmem_start': '0x01da0000',
    'checks': ['source artifact hashes', 'MIPS ELF load and entry',
               'no decompressor overlap with factory bootmem',
               'appended DTB preserved', 'clock preservation argument',
               'RFC cipher vector', 'uImage CRCs', 'encrypted roundtrip',
               'outer gzip expands to exact modern image plus DTB'],
    'remaining_before_boot': [
        'Verify fresh card snapshot, original boot files and candidate sector readback',
        'Use reviewed USB-only profile; AVP coexistence still requires runtime validation',
        'Have an SD restore path and a bounded USB rescue observation procedure',
    ],
}
(out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
