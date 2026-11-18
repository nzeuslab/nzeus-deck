# gma-native-keys only. Read-only: shows the kernel's button devices, then
# prints every press for 60 s while I press buttons (see README.md).
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-keys*|7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_KEYS_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/key-events || { echo KEY_EVENTS_NOT_UPLOADED; exit 1; }
uname -r
echo "== kernel messages about GPIO and keys"
dmesg | grep -i -E 'gpio|keys|input' | tail -n 12
echo "== input devices"
cat /proc/bus/input/devices
echo "== interrupts before"
cat /proc/interrupts
/tmp/nzeus-tools/key-events 60
echo "KEY_EVENTS_EXIT=$?"
echo "== interrupts after"
cat /proc/interrupts
dmesg | tail -n 3
