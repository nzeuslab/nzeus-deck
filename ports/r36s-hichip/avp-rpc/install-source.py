"""Install only the NZEUS AVP RPC probe driver into the pinned PC kernel tree."""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
assert subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() == pin
changes = {
    'drivers/misc/Kconfig': '''
config NZEUS_AVP_RPC
\tbool "NZEUS read-only AVP RPC mailbox probe (HC16xx)"
\tdepends on MACH_HICHIP
\thelp
\t  Polled, read-only display queries to the factory AVP firmware through
\t  the HiChip amprpc mailbox. Nothing is sent until userspace reads the
\t  dis_query sysfs attribute. Experimental diagnostic only.
''',
    'drivers/misc/Makefile': '\nobj-$(CONFIG_NZEUS_AVP_RPC) += nzeus-avp-rpc.o\n',
}
for relative, addition in changes.items():
    original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative])
    patched = original + addition.encode()
    path = tree / relative
    if path.read_bytes() not in (original, patched):
        raise SystemExit('Unrelated edits: ' + str(path))
    path.write_bytes(patched)
(tree / 'drivers/misc/nzeus-avp-rpc.c').write_bytes((root / 'nzeus-avp-rpc.c').read_bytes())
print('Installed NZEUS AVP RPC probe source; no device or boot image changed')
