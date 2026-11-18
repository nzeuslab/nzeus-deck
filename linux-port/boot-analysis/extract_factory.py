"""Reproduce the factory bootloader extraction on the PC, without device access."""
import hashlib
import json
import lzma
import re
import runpy
import struct
from pathlib import Path

root = Path(__file__).resolve().parent
project = root.parent.parent
nor = (project / "reports/mtd0.bin").read_bytes()
assert hashlib.sha256(nor).hexdigest() == "992d09d8cdd4e0b65cbe7de817c5fa4558c8b842685283d33401c6149474d419"
script = root.parent / "ram-handoff/build/upstream-decrypt-tpl.py"
assert hashlib.sha256(script.read_bytes()).hexdigest() == "852863a493a1de015ec42d6ede5bca48c3232e1ecf81c8d4fa49255d778564b7"
crypt = runpy.run_path(str(script))["crypt"]
size, load, source, entry = struct.unpack_from("<4I", nor, 0x20)
offset = source - 0xAFC00000
assert (size, offset) == (0x4A620, 0x4000)
decoded = crypt(nor[offset:offset + size])
# The decoded entry stub passes this LZMA stream to its decompressor.
stream_offset = 0x2E48
decompressor = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
bootloader = decompressor.decompress(decoded[stream_offset:], max_length=16 * 1024 * 1024)
assert decompressor.eof and len(bootloader) == 812464
assert hashlib.sha256(bootloader).hexdigest() == "c95aa3062c308818e4572f65e8c29632ec04cd390cceac0a63d4b053fb4d0ebc"
(root / "factory-tpl.decoded.bin").write_bytes(decoded)
(root / "factory-bootloader.bin").write_bytes(bootloader)
strings = [f"{m.start():#x} {m[0].decode()}" for m in re.finditer(rb"[ -~]{5,}", bootloader)]
(root / "bootloader-strings.txt").write_text("\n".join(strings))
report = dict(nor_sha256=hashlib.sha256(nor).hexdigest(), tpl_offset=hex(offset),
              tpl_size=size, tpl_load=hex(load), tpl_entry=hex(entry),
              lzma_offset=hex(stream_offset), bootloader_base="0x89da0000",
              bootloader_size=len(bootloader),
              bootloader_sha256=hashlib.sha256(bootloader).hexdigest())
(root / "extraction.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS: pinned NOR -> decoded TPL -> decompressed factory bootloader")
