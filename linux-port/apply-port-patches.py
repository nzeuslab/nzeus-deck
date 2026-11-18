"""Apply the reviewed R36S timer adjustment to the pinned modern WSL tree.

Refuse unrelated edits to this file, and allow repeat builds without patch drift.
The SDK initializes the shared south timers. For factory-AVP coexistence use the
main CPU Count/Compare timer, which stock Linux already uses on this board.
"""
import subprocess
import sys
from pathlib import Path

tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
assert subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() == pin
relative = 'arch/mips/Kconfig'
original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative], text=True)
start = original.index('config MACH_HICHIP\n')
end = original.index('\nconfig ', start + 1)
block = original[start:end]
assert block.count('\tselect MIPS_EXTERNAL_TIMER\n') == 1
replacement = block.replace('\tselect MIPS_EXTERNAL_TIMER\n',
    '\t# R36S factory AVP retains the shared south timers.\n'
    '\tselect CEVT_R4K\n\tselect CSRC_R4K\n')
patched = original[:start] + replacement + original[end:]
path = tree / relative
current = path.read_text()
if current not in (original, patched):
    raise SystemExit('Refusing to overwrite unrelated changes in ' + str(path))
if current != patched:
    path.write_text(patched)
print('PASS: pinned HiChip platform selects CPU Count/Compare timers')
