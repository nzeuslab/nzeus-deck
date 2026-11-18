#!/usr/bin/env python3
"""Compare a fake configfs gadget folder (made by nzeus-usb-gadget in the PC
test) with what linux-port/usb-rescue writes, read from usb-rescue itself:
every `echo VALUE > "$gadget/PATH"`, every `mkdir -p "$gadget/DIR"`, the one
`ln -s` and the UDC line. Same files, same values, same link, nothing extra.

  python3 gadget_compare.py linux-port/usb-rescue FAKE/usb_gadget/nzeus_rescue CONTROLLER
Exit 0 = identical; otherwise prints the differences.
"""
import os
import re
import shlex
import sys
from pathlib import Path


def expected_from_usb_rescue(text, controller):
    files, dirs, links = {}, set(), {}
    for line in text.splitlines():
        line = line.strip()
        m = re.fullmatch(r'echo (.+?) > "\$gadget/([^"]+)"', line)
        if m:
            value = ' '.join(shlex.split(m.group(1)))
            files[m.group(2)] = controller if value == '$controller' else value
            continue
        m = re.fullmatch(r'mkdir -p "\$gadget(/[^"]*)?"', line)
        if m:
            parts = (m.group(1) or '').strip('/').split('/')
            for i in range(1, len(parts) + 1):
                if parts[0]:
                    dirs.add('/'.join(parts[:i]))
            continue
        m = re.fullmatch(r'ln -s "\$gadget/([^"]+)" "\$gadget/([^"]+)"', line)
        if m:
            links[m.group(2)] = m.group(1)
            parent = m.group(2).rsplit('/', 1)[0]
            for i in range(1, parent.count('/') + 2):
                dirs.add('/'.join(parent.split('/')[:i]))
    for f in files:
        if '/' in f:
            p = f.rsplit('/', 1)[0]
            for i in range(1, p.count('/') + 2):
                dirs.add('/'.join(p.split('/')[:i]))
    return files, dirs, links


def actual(root):
    files, dirs, links = {}, set(), {}
    for dp, dn, fn in os.walk(root):
        rel = os.path.relpath(dp, root)
        for d in list(dn):
            p = os.path.join(dp, d)
            r = os.path.normpath(os.path.join(rel, d))
            if os.path.islink(p):
                links[r] = os.path.relpath(os.readlink(p), root)
                dn.remove(d)
            else:
                dirs.add(r)
        for f in fn:
            p = os.path.join(dp, f)
            r = os.path.normpath(os.path.join(rel, f))
            if os.path.islink(p):
                links[r] = os.path.relpath(os.readlink(p), root)
            else:
                files[r] = Path(p).read_text().rstrip('\n')
    return files, dirs, links


def main():
    rescue, root, controller = sys.argv[1], sys.argv[2], sys.argv[3]
    ef, ed, el = expected_from_usb_rescue(Path(rescue).read_text(), controller)
    af, ad, al = actual(root)
    problems = []
    if len(ef) < 11 or 'strings/0x409/serialnumber' not in ef or 'UDC' not in ef:
        problems.append(f'could not read usb-rescue properly ({len(ef)} values)')
    for k in sorted(set(ef) | set(af)):
        if ef.get(k) != af.get(k):
            problems.append(f'{k}: usb-rescue {ef.get(k)!r} / ours {af.get(k)!r}')
    if ed != ad:
        problems.append(f'folders differ: only usb-rescue {sorted(ed - ad)}, only ours {sorted(ad - ed)}')
    if el != al:
        problems.append(f'links differ: usb-rescue {el} / ours {al}')
    for p in problems:
        print('DIFF', p)
    print(f'compared {len(ef)} values, {len(ed)} folders, {len(el)} link(s): '
          + ('IDENTICAL' if not problems else f'{len(problems)} difference(s)'))
    return 0 if not problems else 1


if __name__ == '__main__':
    sys.exit(main())
