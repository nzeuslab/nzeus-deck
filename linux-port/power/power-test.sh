# gma-native-power only. Read-only: the driver's start-up line, the standard
# Linux battery/charger files, then 40 s of button events (tap Power, unplug
# and replug the charger). Nothing is switched off here.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_POWER_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/key-events || { echo KEY_EVENTS_NOT_UPLOADED; exit 1; }
uname -r
dmesg | grep -i -E 'r36s-power|power_supply|power-button' | tail -n 6
for s in /sys/class/power_supply/*; do
    echo "== $s"
    for f in type online status capacity capacity_level present technology; do
        test -e "$s/$f" && echo "$f=$(cat "$s/$f")"
    done
done
echo "STOCK_LEVELS $(cat /sys/bus/platform/devices/*.power/stock_levels)"
grep -E '^N: ' /proc/bus/input/devices
/tmp/nzeus-tools/key-events 40
echo "KEY_EVENTS_EXIT=$?"
for s in /sys/class/power_supply/*; do
    echo "AFTER $s status=$(cat "$s/status" 2>/dev/null) online=$(cat "$s/online" 2>/dev/null) capacity=$(cat "$s/capacity" 2>/dev/null)"
done
