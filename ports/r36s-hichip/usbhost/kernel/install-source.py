"""Install the NZEUS USB host changes into the pinned PC kernel tree.

Replaces two files of Siarhei's tree with the versions in this folder:
  drivers/usb/musb/hc16xx.c        musb glue: per-controller data, dr_mode from
                                   the device tree, user role switch (re-creates
                                   the MUSB core), no sleeping in .set_mode
  drivers/phy/phy-hichip-usb.c     optional forced VBUS-valid in host mode
and makes one small edit (CORE_EDITS below) to:
  drivers/base/core.c              device_move() frees the old "glue" folder
Refuses unless the tree is at the pinned commit and each file is either the
pinned original or already exactly our version (no unrelated edits are lost).
  python3 install-source.py KERNEL_TREE [--diff]
--diff only prints the change against the pinned originals (nothing written).
Changes source files on the PC only; no device, card or boot image is touched.
"""
import difflib
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
tree = Path(sys.argv[1])
pin = 'da5f1a22ef2ff5c46aa3f750fa10825813970b09'
files = {
    'drivers/usb/musb/hc16xx.c': root / 'hc16xx.c',
    'drivers/phy/phy-hichip-usb.c': root / 'phy-hichip-usb.c',
    'drivers/base/core.c': None,  # small edit, see CORE_EDITS
}
# device_move() bug in the pinned tree: kobject_move() drops only the moved
# device's own reference to its old parent folder. When that parent is a class
# "glue" folder (e.g. gadget.0/net for usb0), the reference get_device_parent()
# took is never dropped, so the empty folder stays on the class's list. u_ether
# moves usb0 off the gadget on every unbind (gether_detach_gadget). After host
# mode the MUSB core, and with it gadget.0, is rebuilt at the same address; the
# lookup then finds the dead folder and usb0 fails: "kobject_add_internal failed
# for usb0 (error: -2 parent: net)". Fix: drop that reference like device_del().
T, NL = chr(9), chr(10)  # tab and newline (the kernel uses tabs)
CORE_EDITS = [
    (T + 'struct kobject *new_parent_kobj;' + NL + NL + T + 'dev = get_device(dev);' + NL,
     T + 'struct kobject *new_parent_kobj;' + NL
     + T + 'struct kobject *old_glue_kobj;  /* NZEUS: see install-source.py */' + NL + NL
     + T + 'dev = get_device(dev);' + NL),
    (T + 'error = kobject_move(&dev->kobj, new_parent_kobj);' + NL,
     T + 'old_glue_kobj = dev->kobj.parent;' + NL
     + T + 'error = kobject_move(&dev->kobj, new_parent_kobj);' + NL),
    (T + 'put_device(old_parent);' + NL + 'out:' + NL + T + 'device_pm_unlock();' + NL,
     T + '/* NZEUS: free the old glue folder like device_del() does */' + NL
     + T + 'cleanup_glue_dir(dev, old_glue_kobj);' + NL
     + T + 'put_device(old_parent);' + NL + 'out:' + NL + T + 'device_pm_unlock();' + NL),
]


def edited(original):
    text = original.decode()
    for old, new in CORE_EDITS:
        if text.count(old) != 1:
            raise SystemExit('core.c edit does not match the pinned tree: ' + old.strip())
        text = text.replace(old, new)
    return text.encode()


assert subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() == pin
for relative, ours in files.items():
    original = subprocess.check_output(['git', '-C', str(tree), 'show', pin + ':' + relative])
    patched = ours.read_bytes() if ours else edited(original)
    if '--diff' in sys.argv[2:]:
        sys.stdout.writelines(difflib.unified_diff(
            original.decode().splitlines(True), patched.decode().splitlines(True),
            'a/' + relative, 'b/' + relative))
        continue
    path = tree / relative
    if path.read_bytes() not in (original, patched):
        raise SystemExit('Unrelated edits: ' + str(path))
    path.write_bytes(patched)
if '--diff' not in sys.argv[2:]:
    print('Installed NZEUS USB host source (musb glue + PHY + device_move fix); no device or boot image changed')
