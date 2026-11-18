# gma-native-keys only. Read-only status of the running box-demo: uptime, the
# program's log, whether it still runs, and every AVP blank recovery so far.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-keys*|7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_KEYS_KERNEL; exit 1 ;;
esac
echo "UPTIME $(cut -d' ' -f1 /proc/uptime)"
ps | grep box-demo | grep -v grep || echo BOX_DEMO_NOT_RUNNING
echo "== box-demo log (last lines)"
tail -n 12 /tmp/box.log
echo "== AVP blank recoveries"
dmesg | grep -c 'AVP blank'
dmesg | grep 'AVP blank' | tail -n 3
dmesg | tail -n 3
