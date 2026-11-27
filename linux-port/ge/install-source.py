"""Install only the NZEUS HC16xx 2D engine (GE) driver into the pinned PC kernel tree.

Adds drivers/misc/hc16xx-ge.c, include/uapi/linux/hc16xx-ge.h, the device
tree binding and their Kconfig/Makefile lines. Refuses unless the tree is at the pinned commit and the
edited files still start with their pinned content; each addition is appended
once, so other NZEUS drivers may already have added their own lines there.
PC source files only: no device, card or boot image is touched.
  python3 install-source.py KERNEL_TREE
"""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
assert subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() == pin
T = chr(9)
NL = chr(10)
changes = {
    'drivers/misc/Kconfig': NL.join([
        '',
        'config HC16XX_GE',
        T + 'tristate "HiChip HC16xx 2D graphics engine (GE)"',
        T + 'depends on MACH_HICHIP',
        T + 'help',
        T + '  The SoC\'s 2D engine: stretches, blends and copies rectangles',
        T + '  from memory to memory. The driver owns the block\'s clock, reset',
        T + '  and registers and maps one DMA-coherent command queue that a',
        T + '  single process fills; pictures are read from the framebuffer.',
        T + '  Nothing is set up until the device is opened.',
        '']),
    'drivers/misc/Makefile': NL + 'obj-$(CONFIG_HC16XX_GE)\t\t+= hc16xx-ge.o' + NL,
}
for relative, addition in changes.items():
    original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative])
    path = tree / relative
    current = path.read_bytes()
    if not current.startswith(original):
        raise SystemExit('Unrelated edits: ' + str(path))
    if addition.encode() not in current:
        path.write_bytes(current + addition.encode())
(tree / 'drivers/misc/hc16xx-ge.c').write_bytes((root / 'hc16xx-ge.c').read_bytes())
(tree / 'include/uapi/linux/hc16xx-ge.h').write_bytes((root / 'hc16xx-ge.h').read_bytes())
(tree / 'Documentation/devicetree/bindings/gpu/hichip,hc16xx-ge.yaml').write_bytes(
    (root / 'hichip,hc16xx-ge.yaml').read_bytes())
print('Installed HC16xx GE driver source; no device or boot image changed')
