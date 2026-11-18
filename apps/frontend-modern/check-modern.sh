# Modern Linux (gma-native-* kernels) only. Read-only status of nzeus-modern
# (started by run-modern.sh): uptime, is it running, its log, free memory and
# every AVP blank recovery so far. Changes nothing.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-*) ;;
    *) echo REFUSED_NOT_GMA_NATIVE_KERNEL; exit 1 ;;
esac
echo "UPTIME $(cut -d' ' -f1 /proc/uptime)"
ps | grep 'nzeus-modern ' | grep -v grep || echo NZEUS_MODERN_NOT_RUNNING
grep -E "MemTotal|MemAvailable" /proc/meminfo
echo "== nzeus-modern log (last lines)"
tail -n 15 /tmp/nzeus-modern.log
echo "== AVP blank recoveries"
dmesg | grep -c 'AVP blank'
dmesg | tail -n 3
