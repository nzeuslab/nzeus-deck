"""Install only the NZEUS R36S sound card driver (ALSA) into the pinned PC kernel tree.

Adds drivers/char/nzeus-r36s-audio.c plus its Kconfig/Makefile lines.
Refuses unless the tree is at the pinned commit and each changed file is the
pinned original or already exactly ours. PC source files only: no device,
card or boot image is touched.
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
    'drivers/char/Kconfig': NL.join([
        '',
        'config NZEUS_R36S_AUDIO',
        T + 'bool "NZEUS R36S speaker (ALSA, HiChip I2S, pure Linux)"',
        T + 'depends on MACH_HICHIP && GPIOLIB && HAS_DMA && SND=y',
        T + 'select SND_PCM',
        T + 'help',
        T + '  ALSA playback through the HiChip SND0 I2S block to the R36S',
        T + '  TM8211 DAC and speaker amp, without the AVP. Nothing is set up',
        T + '  until the first playback.',
        '']),
    'drivers/char/Makefile': NL + 'obj-$(CONFIG_NZEUS_R36S_AUDIO) += nzeus-r36s-audio.o' + NL,
}
for relative, addition in changes.items():
    original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative])
    patched = original + addition.encode()
    path = tree / relative
    if path.read_bytes() not in (original, patched):
        raise SystemExit('Unrelated edits: ' + str(path))
    path.write_bytes(patched)
(tree / 'drivers/char/nzeus-r36s-audio.c').write_bytes((root / 'nzeus-r36s-audio.c').read_bytes())
print('Installed NZEUS R36S sound card source; no device or boot image changed')
