# Power-off EXPERIMENT (2026-10-10), see pin-drive.c. Run while I watch
# the console: it may switch off during the 15 s hold (that is a result, not a
# failure). First unbinds the nzeus-r36s-power driver for this boot only, so
# the kernel's own button watcher cannot start a shutdown when it sees L14 low
# (battery sysfs and KEY_POWER come back at the next boot). Nothing is mounted
# or written; only GPIO L14 (bit 14) is touched.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-*) ;;
    *) echo REFUSED_NOT_MODERN_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/pin-drive || { echo PIN_DRIVE_NOT_UPLOADED; exit 1; }
grep -q ' /tmp/sd ' /proc/mounts && { echo CARD_MOUNTED_STOP; exit 1; }
echo "UPTIME $(cut -d' ' -f1 /proc/uptime)"
D=/sys/bus/platform/drivers/nzeus-r36s-power
if test -e "$D/188184d0.power"; then
    echo 188184d0.power > "$D/unbind" && echo POWER_DRIVER_UNBOUND
fi
test -e "$D/188184d0.power" && { echo POWER_DRIVER_STILL_BOUND_STOP; exit 1; }
/tmp/nzeus-tools/pin-drive 15 14
echo "L14_TOOL_EXIT=$?"
dmesg | tail -n 4
