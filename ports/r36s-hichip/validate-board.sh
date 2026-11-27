#!/bin/bash
# PC-only check. No ADB, SSH, disk writes, firmware changes, or target execution.
set -euo pipefail
cd "$(dirname "$0")"
for tool in gcc dtc fdtget python3; do command -v "$tool" >/dev/null; done
python3 - <<'PY'
import hashlib, json
from pathlib import Path
manifest = json.loads(Path('upstream-manifest.json').read_text(encoding='utf-8-sig'))
for item in manifest['files']:
    path = Path('upstream') / item['path']
    if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
        raise SystemExit(f'Upstream source changed: {path}')
print('PASS: pinned upstream source hashes')
PY
mkdir -p build/include
# Kernel source uses a symlink here. Provide the same contents for Windows copies.
cp upstream/include/uapi/linux/input-event-codes.h build/include/linux-event-codes.h
for board in sf3000 r36s-e3100-bringup; do
    source="upstream/$board.dts"
    [ "$board" = sf3000 ] || source="$board.dts"
    gcc -E -P -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -Ibuild/include -Iupstream/include -Iupstream "$source" > "build/$board.preprocessed.dts"
    dtc -I dts -O dtb -o "build/$board.dtb" "build/$board.preprocessed.dts" \
        2> "build/$board.warnings.txt"
done
python3 - <<'PY'
import subprocess
from pathlib import Path

dtb = 'build/r36s-e3100-bringup.dtb'
def prop(node, name, kind='s'):
    return subprocess.check_output(['fdtget', '-t', kind, dtb, node, name], text=True).strip()

assert prop('/memory', 'reg', 'x') == '0 2f91e50', 'Unexpected RAM range'
args = prop('/chosen', 'bootargs')
assert 'rdinit=/init' in args and 'root=' not in args and 'mem=' not in args
assert 'clk_ignore_unused' in args.split(), 'Factory AVP clock preservation missing'
assert prop('/usb@18844000', 'dr_mode') == 'peripheral'
assert prop('/usb@18850000', 'status') == 'disabled'
assert prop('/dw-mmc@1884c000', 'max-frequency', 'u') == '20000000'
assert prop('/dw-mmc@1884c000', 'status') == 'disabled'
assert prop('/timer@4', 'status') == 'disabled'
assert prop('/gpio-controller@18800044', 'status') == 'disabled'
assert prop('/pwm@18818410', 'status') == 'disabled'
assert prop('/gpio-keys', 'status') == 'disabled'
assert prop('/serializer-keys', 'status') == 'disabled'
assert prop('/backlight', 'status') == 'disabled'
# Distinguish inherited upstream warnings from errors introduced by this profile.
def warnings(name):
    return [line.split(': Warning ', 1)[1] for line in Path(name).read_text().splitlines()
            if ': Warning ' in line]
assert warnings('build/sf3000.warnings.txt') == warnings('build/r36s-e3100-bringup.warnings.txt')
print('PASS: both device trees compile; draft adds no warnings beyond upstream')
print('PASS: conservative RAM, initramfs-only boot, USB gadget, 20 MHz SD, deferred input/display')
print('BOARD-ONLY CHECK: see verify-artifacts.py for kernel packaging; hardware remains untested')
PY
sha256sum build/r36s-e3100-bringup.dtb
