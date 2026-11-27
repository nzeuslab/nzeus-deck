"""PC-only codec for the image wrapper found in this factory V2.7 bootloader.

No device access or deployment. Header: 12-byte nonce, LE32 size, ChaCha20 data.
The counter starts at zero. Decoded data is a normal legacy uImage.
"""
import struct
import zlib
import hashlib
from pathlib import Path

MASK = 0xFFFFFFFF


def rotate(value, count):
    return ((value << count) | (value >> (32 - count))) & MASK


def block(key, nonce, counter):
    state = list(struct.unpack("<4I", b"expand 32-byte k"))
    state += list(struct.unpack("<8I", key)) + [counter]
    state += list(struct.unpack("<3I", nonce))
    work = state.copy()

    def quarter(a, b, c, d):
        work[a] = (work[a] + work[b]) & MASK
        work[d] = rotate(work[d] ^ work[a], 16)
        work[c] = (work[c] + work[d]) & MASK
        work[b] = rotate(work[b] ^ work[c], 12)
        work[a] = (work[a] + work[b]) & MASK
        work[d] = rotate(work[d] ^ work[a], 8)
        work[c] = (work[c] + work[d]) & MASK
        work[b] = rotate(work[b] ^ work[c], 7)

    for _ in range(10):
        for indices in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                        (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
            quarter(*indices)
    return struct.pack("<16I", *[(x + y) & MASK for x, y in zip(work, state)])


def crypt(data, key, nonce):
    result = bytearray(len(data))
    for offset in range(0, len(data), 64):
        stream = block(key, nonce, offset // 64)
        chunk = data[offset:offset + 64]
        result[offset:offset + len(chunk)] = bytes(a ^ b for a, b in zip(chunk, stream))
    return bytes(result)


def factory_key(bootloader):
    if hashlib.sha256(bootloader).hexdigest() != "c95aa3062c308818e4572f65e8c29632ec04cd390cceac0a63d4b053fb4d0ebc":
        raise ValueError("This codec is pinned to the inspected E3100 factory bootloader")
    # Factory function 0x89dab188 reads this table and applies this byte transform.
    table = bootloader[0x9CC98:0x9CCB8]
    if len(table) != 32:
        raise ValueError("Factory bootloader is too short")
    key = bytearray()
    for i, value in enumerate(table):
        value ^= ((0x33 + 7 * i) & 255) ^ 0x55
        key.append((((value << 5) | (value >> 3)) ^ 0xAA) & 255)
    return bytes(key)


def decode(wrapped, key):
    if len(wrapped) < 16:
        raise ValueError("Truncated wrapper")
    length = struct.unpack_from("<I", wrapped, 12)[0]
    if length == 0 or length != len(wrapped) - 16:
        raise ValueError("Wrapper length does not match file")
    return crypt(wrapped[16:], key, wrapped[:12])


def encode(image, key, nonce):
    if len(nonce) != 12:
        raise ValueError("Need a 12-byte nonce")
    return nonce + struct.pack("<I", len(image)) + crypt(image, key, nonce)


def check_uimage(image):
    if len(image) < 64:
        raise ValueError("Truncated uImage")
    magic, hcrc, timestamp, size, load, entry, dcrc, os_id, arch, kind, compression, name = \
        struct.unpack(">7I4B32s", image[:64])
    if magic != 0x27051956 or size != len(image) - 64:
        raise ValueError("Invalid uImage magic or size")
    header = bytearray(image[:64])
    header[4:8] = b"\0" * 4
    if zlib.crc32(header) != hcrc:
        raise ValueError("Header CRC mismatch")
    if zlib.crc32(image[64:]) != dcrc:
        raise ValueError("Payload CRC mismatch")
    return dict(size=size, load=hex(load), entry=hex(entry), timestamp=timestamp,
                os=os_id, arch=arch, type=kind, compression=compression,
                name=name.rstrip(b"\0").decode(errors="replace"),
                header_crc=hex(hcrc), data_crc=hex(dcrc))


def test_rfc_block():
    # RFC 8439 section 2.3.2, independent of the console files.
    expected = bytes.fromhex(
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e")
    assert block(bytes(range(32)), bytes.fromhex("000000090000004a00000000"), 1) == expected


if __name__ == "__main__":
    import hashlib
    import json
    root = Path(__file__).resolve().parent
    test_rfc_block()
    wrapped = (root.parent / "ram-handoff/build/stock-vmlinux.uImage").read_bytes()
    key = factory_key((root / "factory-bootloader.bin").read_bytes())
    image = decode(wrapped, key)
    metadata = check_uimage(image)
    assert encode(image, key, wrapped[:12]) == wrapped, "Repacking must reproduce stock bytes"
    (root / "stock-kernel.decoded.uImage").write_bytes(image)
    metadata.update(wrapper_size=len(wrapped), nonce=wrapped[:12].hex(),
                    original_sha256=hashlib.sha256(wrapped).hexdigest(),
                    decoded_sha256=hashlib.sha256(image).hexdigest(),
                    roundtrip_exact=True, rfc_block_pass=True)
    (root / "stock-kernel-analysis.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))
