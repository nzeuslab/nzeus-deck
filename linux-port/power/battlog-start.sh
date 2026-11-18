# gma-native-power/pure. Starts a BACKGROUND battery logger: every 60 s one
# line "uptime stock_levels capacity status" into /tmp/battlog.txt (RAM, tiny:
# about 60 bytes a line). Unplug the charger and let it run down to build the
# real discharge curve for an accurate percentage. Read it with battlog-show.sh.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_POWER_KERNEL; exit 1 ;;
esac
L=$(ls /sys/bus/platform/devices/*.power/stock_levels 2>/dev/null)
test -n "$L" || { echo NO_STOCK_LEVELS; exit 1; }
ps | grep 'battlog-loop' | grep -v grep && { echo ALREADY_RUNNING; exit 0; }
cat > /tmp/battlog-loop.sh <<EOS
while true; do
    echo "\$(cut -d' ' -f1 /proc/uptime) \$(cat $L) cap=\$(cat /sys/class/power_supply/battery/capacity) \$(cat /sys/class/power_supply/battery/status)" >> /tmp/battlog.txt
    sleep 60
done
EOS
setsid sh /tmp/battlog-loop.sh > /dev/null 2>&1 &
sleep 1
echo "BATTLOG_STARTED"
cat /tmp/battlog.txt
