# Power-off EXPERIMENT 2 (2026-10-10), see pin-drive.c. Run while I watch:
# drives L01 (the stock device tree's "power-pin") low for up to 5 s; the
# console switching off during the hold is the hoped-for result. Only GPIO
# L01 (bit 1) is touched; nothing is mounted or written.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-*) ;;
    *) echo REFUSED_NOT_MODERN_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/pin-drive || { echo PIN_DRIVE_NOT_UPLOADED; exit 1; }
grep -q ' /tmp/sd ' /proc/mounts && { echo CARD_MOUNTED_STOP; exit 1; }
sync
echo "UPTIME $(cut -d' ' -f1 /proc/uptime)"
/tmp/nzeus-tools/pin-drive 5 1
echo "PIN_TOOL_EXIT=$?"
dmesg | tail -n 4
