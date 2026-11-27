#!/usr/bin/env python3
"""Print the entry list of a kernel initramfs (initramfs.cpio.gz) in the
format of initramfs-contents.txt (one line per entry: kind, path, permissions,
owner, link target or device numbers), plus SHA-256 of every regular file
with --hashes. Reads only the given file.

  python3 list-initramfs.py display/build-sdroot/initramfs.cpio.gz > sdroot/initramfs-contents.txt
"""
import gzip
import hashlib
import sys

KINDS = {0o040000: 'dir', 0o100000: 'file', 0o120000: 'slink', 0o020000: 'nod'}


def entries(path):
    data = gzip.decompress(open(path, 'rb').read())
    pos = 0
    while True:
        assert data[pos:pos + 6] == b'070701'
        f = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        pos += 110
        name = data[pos:pos + f[11] - 1].decode().lstrip('/')
        pos = (pos + f[11] + 3) & ~3
        content = data[pos:pos + f[6]]
        pos = (pos + f[6] + 3) & ~3
        if name == 'TRAILER!!!':
            return
        yield name, f[1], f[2], f[3], f[9], f[10], content


def main():
    hashes = '--hashes' in sys.argv
    path = [a for a in sys.argv[1:] if a != '--hashes'][0]
    for name, mode, uid, gid, rmaj, rmin, content in sorted(entries(path)):
        kind = KINDS[mode & 0o170000]
        line = '%s /%s %04o %d %d' % (kind, name, mode & 0o7777, uid, gid)
        if kind == 'slink':
            line += ' -> ' + content.rstrip(b'\0').decode()
        if kind == 'nod':
            line += ' c %d %d' % (rmaj, rmin)
        if hashes and kind == 'file':
            line += '  sha256=' + hashlib.sha256(content).hexdigest()
        print(line)


if __name__ == '__main__':
    main()
