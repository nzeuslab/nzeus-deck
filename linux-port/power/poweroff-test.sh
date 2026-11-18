# gma-native-power only. SWITCHES THE CONSOLE OFF through the new driver
# (reboot(RB_POWER_OFF) -> L14 driven low). Nothing is mounted or written.
# Success = the USB rescue disappears and the console is off; the PC side
# then reports an incomplete transcript, which is expected.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    7.2.0-rc5-nzeus-gma-native-net*|7.2.0-rc5-nzeus-gma-native-sdroot*) ;;   # built on pure
    *) echo REFUSED_NOT_POWER_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/nz-poweroff || { echo NZ_POWEROFF_NOT_UPLOADED; exit 1; }
grep -q ' /tmp/sd ' /proc/mounts && { echo CARD_MOUNTED_STOP; exit 1; }
echo "UPTIME $(cut -d' ' -f1 /proc/uptime)"
/tmp/nzeus-tools/nz-poweroff --yes
echo "STILL_RUNNING_AFTER_POWEROFF exit=$?"
dmesg | tail -n 5
