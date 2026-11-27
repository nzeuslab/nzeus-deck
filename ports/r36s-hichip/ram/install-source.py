"""Install only the NZEUS R36S AVP-free RAM driver into the pinned PC kernel tree."""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
assert subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() == pin
changes = {
    'drivers/soc/Kconfig': '''
config NZEUS_R36S_RAM
\tbool "NZEUS R36S locked AVP-area RAM, released only when the AVP is off"
\tdepends on MACH_HICHIP && OF_RESERVED_MEM
\thelp
\t  Keeps the AVP firmware's memory reserved at boot and gives it to Linux
\t  only after an uncached check shows the AVP is not running.
''',
    'drivers/soc/Makefile': '\nobj-$(CONFIG_NZEUS_R36S_RAM) += nzeus-ram.o\n',
}
for relative, addition in changes.items():
    original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative])
    patched = original + addition.encode()
    path = tree / relative
    if path.read_bytes() not in (original, patched):
        raise SystemExit('Unrelated edits: ' + str(path))
    path.write_bytes(patched)
(tree / 'drivers/soc/nzeus-ram.c').write_bytes((root / 'nzeus-ram.c').read_bytes())
print('Installed NZEUS R36S RAM source; no device or boot image changed')
