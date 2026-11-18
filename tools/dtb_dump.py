# Prints a device tree blob (.dtb) as readable text, like "dtc -I dtb -O dts".
# A device tree is the file that tells Linux what hardware exists and where.
# Usage: python dtb_dump.py file.dtb
import struct
import sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def show_value(name, raw):
    """Turn a property's raw bytes into something readable."""
    if not raw:
        return ""
    # text strings (possibly several, separated by zero bytes)
    if raw[-1] == 0 and all(32 <= b < 127 or b == 0 for b in raw) and raw[0] != 0:
        parts = raw[:-1].split(b"\0")
        if all(parts):
            return " = " + ", ".join('"%s"' % p.decode() for p in parts)
    # groups of 32-bit numbers
    if len(raw) % 4 == 0:
        cells = struct.unpack(">%dI" % (len(raw) // 4), raw)
        return " = <" + " ".join("0x%x" % c for c in cells) + ">"
    return " = [" + raw.hex() + "]"


def dump(path):
    data = open(path, "rb").read()
    magic, total, off_struct, off_strings = struct.unpack(">IIII", data[:16])
    assert magic == 0xD00DFEED, "not a DTB"
    pos, depth = off_struct, 0

    def string_at(off):
        end = data.index(b"\0", off_strings + off)
        return data[off_strings + off:end].decode()

    while True:
        token = struct.unpack(">I", data[pos:pos + 4])[0]
        pos += 4
        if token == FDT_BEGIN_NODE:
            end = data.index(b"\0", pos)
            name = data[pos:end].decode() or "/"
            pos = (end + 4) & ~3                   # names are padded to 4 bytes
            print("  " * depth + name + " {")
            depth += 1
        elif token == FDT_END_NODE:
            depth -= 1
            print("  " * depth + "};")
        elif token == FDT_PROP:
            length, name_off = struct.unpack(">II", data[pos:pos + 8])
            pos += 8
            raw = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
            name = string_at(name_off)
            print("  " * depth + name + show_value(name, raw) + ";")
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            break


dump(sys.argv[1])
