#!/usr/bin/env python3
"""The /usb-rescue kernel-name guard must admit gma-native-sdroot.

The 2026-10-09 lesson (display/bootlog/test-usb-admission.py): a renamed
kernel whose name the guard did not admit booted without any USB shell.
This runs the EXACT guard of ports/r36s-hichip/usb-rescue (everything before
'mount -t configfs', so no USB command is executed) for the sdroot kernel
name and the earlier names, with the PC's sh (uname overridden) and - if
given - with the console's MIPS BusyBox under qemu-mipsel (QEMU_UNAME).

  python3 test-usb-admission.py [--busybox MIPS_BUSYBOX] [--report FILE]
"""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--busybox')
ap.add_argument('--report')
args = ap.parse_args()
script = (HERE.parent / 'usb-rescue').read_text()
guard = script.split('mount -t configfs', 1)[0]
assert guard.count('case ') == 1 and guard.count('esac') == 1
versions = {
    '7.2.0-rc5-nzeus-gma-native-sdroot+': True,      # this build (LOCALVERSION + dirty-tree '+')
    '7.2.0-rc5-nzeus-gma-native-sdroot': True,
    '7.2.0-rc5-nzeus-gma-native-net+': True,
    '7.2.0-rc5-nzeus-gma-native-pure+': True,
    '7.2.0-rc5-nzeus-gma-native-usbfix+': True,
    '7.2.0-rc5-nzeus-screenlog-usbfix+': True,
    '7.2.0-rc5-nzeus-experimental+': True,
    '4.4.186': False,                                 # stock NZEUS console kernel
    '3.18.24': False,
    '7.2.0-rc5-stock': False,
}
results = []
with tempfile.TemporaryDirectory(prefix='nz-usb-guard-') as tmp:
    (Path(tmp) / 'guard.sh').write_text(guard + 'echo ADMITTED\n')
    for version, accepted in versions.items():
        code = 'uname() { printf "%s\\n" "$TEST_RELEASE"; }\n' + guard + '\necho ADMITTED\n'
        r = subprocess.run(['sh', '-c', code], env={'PATH': '/usr/bin:/bin', 'TEST_RELEASE': version},
                           capture_output=True, text=True)
        ok = (r.returncode == 0 and 'ADMITTED' in r.stdout) == accepted
        if not accepted:
            ok = ok and r.returncode == 1 and 'Refusing USB setup' in r.stderr
        row = {'version': version, 'admitted': accepted, 'pc_sh': ok}
        if args.busybox:
            q = subprocess.run([shutil.which('qemu-mipsel') or 'qemu-mipsel', args.busybox, 'sh',
                                str(Path(tmp) / 'guard.sh')], env=dict(os.environ, QEMU_UNAME=version),
                               capture_output=True, text=True)
            row['mips_busybox'] = (q.returncode == 0 and 'ADMITTED' in q.stdout) == accepted
            ok = ok and row['mips_busybox']
        row['ok'] = ok
        results.append(row)
        print('%s %-40s admitted=%s' % ('PASS' if ok else 'FAIL', version, accepted))
passed = sum(r['ok'] for r in results)
print('USB_ADMISSION_TESTS %d/%d passed' % (passed, len(results)))
if args.report:
    Path(args.report).write_text(json.dumps({'status': 'PASS_PC_GUARD_ONLY' if passed == len(results) else 'FAIL',
                                             'cases': results, 'limits': ['No USB hardware exercised']}, indent=2) + '\n')
raise SystemExit(0 if passed == len(results) else 1)
