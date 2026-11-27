"""Install only our GMA additions into the pinned PC kernel source tree."""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
assert subprocess.check_output(['git','-C',str(tree),'rev-parse','HEAD'],text=True).strip()==pin
changes = {
    'drivers/video/fbdev/Kconfig': '''
config FB_HC16XX_GMA_FIRMWARE
    bool "Experimental E3100 firmware-timed GMA framebuffer"
    depends on FB=y && MACH_HICHIP
    depends on !FRAMEBUFFER_CONSOLE
    select FB_DMAMEM_HELPERS
    help
      Fixed 640x480 ARGB8888 scanout using firmware-initialized panel timing.
      Built-in diagnostic only. No display activation until explicit unblank.
''',
    'drivers/video/fbdev/Makefile': '\nobj-$(CONFIG_FB_HC16XX_GMA_FIRMWARE) += hc16xx-gma-fb.o\n',
}
for relative, addition in changes.items():
    original = subprocess.check_output(['git','-C',str(tree),'show',pin+':'+relative])
    patched = original + addition.encode()
    path=tree/relative
    legacy = original + addition.replace('ARGB8888', 'RGB565').encode()
    if path.read_bytes() not in (original,patched,legacy):
        raise SystemExit('Unrelated edits: '+str(path))
    path.write_bytes(patched)
(tree/'drivers/video/fbdev/hc16xx-gma-fb.c').write_bytes((root/'hc16xx-gma-fb.c').read_bytes())
print('Installed experimental GMA source; no device or boot image changed')
