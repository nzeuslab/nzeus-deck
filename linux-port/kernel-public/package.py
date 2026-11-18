#!/usr/bin/env python3
"""Packages the public kernel and the AVP stand-in for the factory bootloader. PC only.

  python3 linux-port/kernel-public/package.py --elf WORK/nzeus-kernel.elf \\
          --bootloader FACTORY_BOOTLOADER.bin --out DIR

Writes DIR/advapi32.dll (the kernel) and DIR/ApplicationFrame.dll (the AVP
stand-in), both in the factory's ChaCha20 wrapper, plus DIR/verification.json.
FACTORY_BOOTLOADER is YOUR console's factory bootloader (extracted from your own
NOR backup); the wrapper key is computed from it and never written anywhere.
Same recipe as my daily-kernel packaging (fpudiag/package-fpudiag.py)
and purelinux/make-avp-stub.py. Never touches a card or the console.
"""
import argparse
import gzip
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'boot-analysis'))
from image_codec import check_uimage, decode, encode, factory_key, test_rfc_block  # noqa: E402

LOAD = 0x80100000          # where the kernel runs
SHIM = 0x81000000          # the bootloader jumps here; the shim jumps to kernel_entry


def run(*command):
    return subprocess.check_output(list(map(str, command)), text=True, stderr=subprocess.DEVNULL)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def wrap(plain, key, tag):
    """The factory container: ChaCha20 with a nonce that depends only on the content."""
    check_uimage(plain)
    wrapped = encode(plain, key, hashlib.sha256(tag + plain).digest()[:12])
    assert decode(wrapped, key) == plain
    return wrapped


def kernel_image(elfpath, dtspath, tmp):
    elf = elfpath.read_bytes()
    h = struct.unpack_from('<HHIIIIIHHHHHH', elf, 16)
    assert elf[:7] == b'\x7fELF\x01\x01\x01' and h[1] == 8, 'not a little-endian MIPS ELF'
    loads = [s for s in (struct.unpack_from('<8I', elf, h[4] + i * h[8]) for i in range(h[9])) if s[0] == 1]
    assert len(loads) == 1, 'expected one loadable segment'
    _, offset, address, physical, size, memsize, _, _ = loads[0]
    assert address == physical == LOAD
    raw = elf[offset:offset + size]
    symbols = {p[2]: int(p[0], 16) for line in run('mipsel-linux-gnu-nm', '-n', elfpath).splitlines()
               if len(p := line.split()) == 3}
    assert symbols['kernel_entry'] == h[3] and symbols['__appended_dtb'] == address + size

    # the board description: compiled from board.dts (the daily one without blkdevparts=)
    dtb_path = tmp / 'board.dtb'
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', dtb_path, dtspath], check=True)
    dtb = dtb_path.read_bytes()
    bootargs = run('fdtget', '-t', 's', dtb_path, '/chosen', 'bootargs').split()
    assert 'nzclk=900' in bootargs and not any(a.startswith('blkdevparts=') for a in bootargs), bootargs
    assert symbols['__appended_dtb'] + len(dtb) <= symbols['__bss_start']

    # entry shim at 0x81000000 -> kernel_entry (same template as the daily kernel)
    asm = (HERE / 'entry.S.in').read_text().replace('@ENTRY_HI@', hex(h[3] >> 16)).replace('@ENTRY_LO@', hex(h[3] & 65535))
    (tmp / 'entry.S').write_text(asm)
    (tmp / 'entry.ld').write_text('ENTRY(_start)\nSECTIONS { . = 0x81000000; .text : { *(.text) } '
                                  '/DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.gnu.attributes) } }\n')
    run('mipsel-linux-gnu-as', '-mips32r2', '-EL', '-o', tmp / 'entry.o', tmp / 'entry.S')
    run('mipsel-linux-gnu-ld', '-EL', '-T', tmp / 'entry.ld', '-o', tmp / 'entry.elf', tmp / 'entry.o')
    run('mipsel-linux-gnu-objcopy', '-O', 'binary', tmp / 'entry.elf', tmp / 'entry.bin')
    stub = (tmp / 'entry.bin').read_bytes()
    assert len(stub) < 4096 and address + memsize < SHIM and len(raw + dtb) < 0xf00000
    payload_raw = (raw + dtb).ljust(0xf00000, b'\0') + stub
    assert 0x100000 + len(payload_raw) < 0x1da0000          # below the bootloader's heap
    payload = gzip.compress(payload_raw, compresslevel=9, mtime=0)
    header = bytearray(struct.pack('>7I4B32s', 0x27051956, 0, 0, len(payload), address, SHIM,
                                   zlib.crc32(payload), 5, 5, 2, 1, b'NZEUS Deck public kernel'))
    struct.pack_into('>I', header, 4, zlib.crc32(header))
    return bytes(header) + payload, bootargs


def avp_stand_in(tmp):
    """Five instructions: write 2 to 0xb880006b ("AVP ready") and return to the bootloader."""
    load = 0xA1100000
    (tmp / 'avp.S').write_text('    .set noreorder\n    .set nomacro\n    .text\n    .globl _start\n_start:\n'
                               '    lui   $t0, 0xb880\n    addiu $t1, $zero, 2\n    sb    $t1, 0x6b($t0)\n'
                               '    jr    $ra\n    nop\n')
    run('mipsel-linux-gnu-as', '-mips32r2', '-EL', '-o', tmp / 'avp.o', tmp / 'avp.S')
    (tmp / 'avp.ld').write_text('ENTRY(_start)\nSECTIONS { . = 0x%08x; .text : { *(.text) } '
                                '/DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.gnu.attributes) } }\n' % load)
    run('mipsel-linux-gnu-ld', '-EL', '-T', tmp / 'avp.ld', '-o', tmp / 'avp.elf', tmp / 'avp.o')
    run('mipsel-linux-gnu-objcopy', '-O', 'binary', tmp / 'avp.elf', tmp / 'avp.bin')
    code = (tmp / 'avp.bin').read_bytes()
    assert code[:20] == struct.pack('<5I', 0x3c08b880, 0x24090002, 0xa109006b, 0x03e00008, 0) and not any(code[20:])
    header = bytearray(struct.pack('>7I4B32s', 0x27051956, 0, 0, len(code), load, load,
                                   zlib.crc32(code), 17, 5, 1, 0, b'NZEUS AVP stand-in (no AVP)'))
    struct.pack_into('>I', header, 4, zlib.crc32(header))
    return bytes(header) + code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', type=Path, required=True)
    ap.add_argument('--bootloader', type=Path, required=True)
    ap.add_argument('--dts', type=Path, default=HERE / 'board.dts')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    test_rfc_block()
    key = factory_key(a.bootloader.read_bytes())
    a.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        kplain, bootargs = kernel_image(a.elf, a.dts, tmp)
        aplain = avp_stand_in(tmp)
    kernel = wrap(kplain, key, b'NZEUS-PUBLIC-KERNEL-v1')
    avp = wrap(aplain, key, b'NZEUS-AVP-STUB-v1')
    (a.out / 'advapi32.dll').write_bytes(kernel)
    (a.out / 'ApplicationFrame.dll').write_bytes(avp)
    report = dict(status='PC_PACKAGED_NOT_INSTALLED',
                  advapi32_dll=dict(sha256=sha(kernel), bytes=len(kernel), bootargs=' '.join(bootargs),
                                    elf_sha256=sha(a.elf.read_bytes())),
                  ApplicationFrame_dll=dict(sha256=sha(avp), bytes=len(avp)),
                  notes=['wrapper key computed from the factory bootloader and never written',
                         'no blkdevparts= (the copy-and-go boot finds the FAT32 itself)'])
    (a.out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
