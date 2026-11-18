# gma-native-keys only. Starts box-demo for 25 minutes in the BACKGROUND and
# returns at once: the first interactive program AND the long display test
# (one owner of /dev/fb0 the whole time). RAM-only, no card or flash writes.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-keys*|7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_KEYS_KERNEL; exit 1 ;;
esac
test "$(cat /sys/class/graphics/fb0/name)" = hc16xx-gma-argb || { echo FB0_NOT_GMA; exit 1; }
test -x /tmp/nzeus-tools/box-demo || { echo BOX_DEMO_NOT_UPLOADED; exit 1; }
ps | grep -E 'box-demo|fb-pattern' | grep -v grep && { echo ANOTHER_SCREEN_PROGRAM_RUNNING_STOP; exit 1; }
/tmp/nzeus-tools/box-demo 1500 > /tmp/box.log 2>&1 &
echo "BOX_DEMO_STARTED pid=$! uptime=$(cut -d' ' -f1 /proc/uptime)"
sleep 2
cat /tmp/box.log
