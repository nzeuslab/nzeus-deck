"""Install only the NZEUS R36S battery/charger driver into the pinned PC kernel tree."""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
assert subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() == pin
changes = {
    'drivers/power/supply/Kconfig': '''
config NZEUS_R36S_POWER
\tbool "NZEUS R36S battery, charger and power button"
\tdepends on MACH_HICHIP && POWER_SUPPLY
\thelp
\t  Reports the R36S battery level and charger state from the SAR ADC
\t  samples the factory bootloader leaves running (read-only), and the
\t  power button on GPIO L14 (hold 3 s = power off, which drives L14 low).
''',
    'drivers/power/supply/Makefile': '\nobj-$(CONFIG_NZEUS_R36S_POWER) += nzeus-r36s-power.o\n',
}
for relative, addition in changes.items():
    original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative])
    patched = original + addition.encode()
    path = tree / relative
    if path.read_bytes() not in (original, patched):
        raise SystemExit('Unrelated edits: ' + str(path))
    path.write_bytes(patched)
(tree / 'drivers/power/supply/nzeus-r36s-power.c').write_bytes((root / 'nzeus-r36s-power.c').read_bytes())
print('Installed NZEUS R36S power source; no device or boot image changed')
