#!/bin/bash
# test-public-image.sh WORK_DIR - checks the public (copy-and-go) images that
#   build-debian.sh WORK_DIR --reuse --public-fatroot
# made in WORK_DIR/out (nzroot.img, nzdata.img) from WORK_DIR/rootfs.
# Read-only (debugfs reads the images; nothing is mounted). PC only.
# Env: NZ_PRIVATE_WORDS_DIR = folder with my private word lists (for the privacy scan).
set -u
W=${1:?usage: test-public-image.sh WORK_DIR}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$W/rootfs
R=$W/out/nzroot.img
D=$W/out/nzdata.img
pass=0; fail=0
check() {  # NAME CONDITION-RESULT(0/1) [DETAIL]
    if test "$2" = 0; then echo "PASS $1"; pass=$((pass + 1)); else echo "FAIL $1 ${3:-}"; fail=$((fail + 1)); fi
}
cat_img() { debugfs -R "cat $2" "$1" 2>/dev/null; }

# 1. the exact sizes the installer and nz-fatroot expect, and the labels
rs=$(stat -c %s "$R" 2>/dev/null || echo 0); ds=$(stat -c %s "$D" 2>/dev/null || echo 0)
rl=$(e2label "$R" 2>/dev/null); dl=$(e2label "$D" 2>/dev/null)
test "$rs" = 3758096384 && test "$ds" = 1073741824 && test "$rl" = nzroot && test "$dl" = nzdata
check public_image_sizes $? "root=$rs/$rl data=$ds/$dl"

# 2. fstab: root is nz-fatroot's loop device; no card partitions (p1/p4 don't exist here)
fstab=$(cat_img "$R" /etc/fstab)
echo "$fstab" | grep -qE '^/dev/loop1[[:space:]]+/[[:space:]]+ext4' && ! echo "$fstab" | grep -qE '^[^#]*mmcblk0p'
check public_fstab_has_no_p4 $? "$(echo "$fstab" | grep -v '^#')"

# 3. the root check accepts the loop device and nothing is "never write" (FAT32 is written on purpose)
conf=$(cat_img "$R" /etc/nzeus/sdroot.conf)
echo "$conf" | grep -qx 'NZ_ROOT_DEV=/dev/loop1' && echo "$conf" | grep -qx 'NZ_NEVER_WRITE=""'
check public_sdroot_conf $?

# 4. no blkdevparts= line in the NZEUS places (my card's serial (CID) is one of my
#    private words, so check 5 catches it without it being written here)
hits=$(grep -rlaE 'blkdevparts=' "$ROOT/etc" "$ROOT/usr/local" "$ROOT/root" 2>/dev/null)
test -d "$ROOT/etc" && test -z "$hits"          # (a missing root folder must not pass)
check public_no_cid_no_blkdevparts $? "$hits"

# 5. my privacy scan on the root folder the image was made from
out=$(NZ_PRIVATE_WORDS_DIR=${NZ_PRIVATE_WORDS_DIR:-} bash "$HERE/privacy-scan-image.sh" "$ROOT" 2>&1)
check public_privacy_scan_clean $? "$(echo "$out" | grep FAIL)"

# 6. nobody's SSH key inside
keys=$(cat_img "$R" /root/.ssh/authorized_keys)
test -n "$fstab" && test -z "$keys"          # (an unreadable image must not pass)
check public_no_authorized_keys $?

# 7. the data image is empty apart from the Deck's folder: no settings, so Claude/WhatsApp have no PC set
top=$(debugfs -R "ls -p /" "$D" 2>/dev/null | awk -F/ 'NF > 5 { print $6 }')
listing=$(echo "$top" | grep -vxE '\.|\.\.|lost\+found|nzeus' | tr '\n' ' ')
inside=$(debugfs -R "ls -p /nzeus" "$D" 2>/dev/null | awk -F/ 'NF > 5 { print $6 }' | grep -vxE '\.|\.\.' | tr '\n' ' ')
echo "$top" | grep -qx nzeus && test -z "$listing" && test -z "$inside"
check public_claude_whatsapp_unconfigured $? "top: $listing nzeus: $inside"

# 8. the welcome text explains /sdcard (the card's own files)
cat_img "$R" /etc/motd | grep -q '/sdcard'
check public_motd_mentions_sdcard $?

# 9. the boot log on the card (nzeus-bootlog) starts FIRST, before every other start-up step,
#    so a start that freezes anywhere still leaves its last lines in nzeus/boot-log.txt
rcs=$(debugfs -R "ls -p /etc/rcS.d" "$R" 2>/dev/null | awk -F/ 'NF > 5 { print $6 }' | grep '^S[0-9]' | sort)
first=$(echo "$rcs" | head -n 1)
script=$(cat_img "$R" /etc/init.d/nzeus-bootlog)
case "$first" in S[0-9][0-9]nzeus-bootlog) ok=0 ;; *) ok=1 ;; esac
test "$ok" = 0 && test "$(echo "$rcs" | grep -c 'nzeus-bootlog$')" = 1 && test "$(echo "$rcs" | grep -c '^S01')" -ge 1 \
    && test "${first:1:2}" -lt "$(echo "$rcs" | grep -v nzeus-bootlog | head -n 1 | cut -c2-3)" \
    && echo "$script" | sh -n && echo "$script" | grep -q 'boot-log.txt'
check public_boot_log_starts_first $? "first: $first; rcS: $(echo "$rcs" | head -n 4 | tr '\n' ' ')"

echo "$pass/$((pass + fail)) passed"
test "$fail" = 0
