# Modern Linux (gma-native-* kernels) only. Stops nzeus-modern early with
# SIGTERM: it closes NZEUS and hands the screen back to the firmware
# (FIRMWARE_LAYER_RESTORE_REQUESTED in the log). Never a forced kill.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-*) ;;
    *) echo REFUSED_NOT_GMA_NATIVE_KERNEL; exit 1 ;;
esac
test -s /tmp/nzeus-modern.pid || { echo NOT_STARTED_BY_RUN_MODERN; exit 1; }
PID=$(cat /tmp/nzeus-modern.pid)
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID"
    n=0
    while kill -0 "$PID" 2>/dev/null; do
        n=$((n + 1))
        test $n -ge 10 && { echo STILL_RUNNING_AFTER_10_S; exit 1; }
        sleep 1
    done
    echo NZEUS_MODERN_STOPPED
else
    echo NZEUS_MODERN_WAS_NOT_RUNNING
fi
: > /tmp/nzeus-modern.pid                    # (emptied: the boot BusyBox has no rm)
tail -n 5 /tmp/nzeus-modern.log
