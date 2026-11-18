# Modern Linux (gma-native-* kernels) only. Starts NZEUS Deck (nzeus-modern)
# in the BACKGROUND with a time limit and returns at once. RAM-only: NZEUS's
# files go to /tmp/nzeus-data; no card, flash or raw-device writes.
#
#   sh run-modern.sh [SECONDS]    1..3600, default 600 (rescue-command.py passes
#                                 no arguments: edit LIMIT below for a longer run)
#   log: /tmp/nzeus-modern.log    status: check-modern.sh    stop early: stop-modern.sh
#
# Written for the boot BusyBox: no `[`, tr, dd, od, sha256sum or timeout.
LIMIT=${1:-600}
LOG=/tmp/nzeus-modern.log

case "$LIMIT" in
    ''|*[!0-9]*) echo "BAD_SECONDS (usage: sh run-modern.sh [SECONDS 1..3600])"; exit 1 ;;
esac
test "$LIMIT" -ge 1 || { echo BAD_SECONDS_1_TO_3600; exit 1; }
test "$LIMIT" -le 3600 || { echo BAD_SECONDS_1_TO_3600; exit 1; }
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-*) ;;
    *) echo REFUSED_NOT_GMA_NATIVE_KERNEL; exit 1 ;;
esac
test "$(cat /sys/class/graphics/fb0/name)" = hc16xx-gma-argb || { echo FB0_NOT_GMA; exit 1; }
test -x /tmp/nzeus-tools/nzeus-modern || { echo NZEUS_MODERN_NOT_UPLOADED; exit 1; }
# ONE owner of /dev/fb0 at a time (two owners spoil long tests)
ps | grep -E 'box-demo|fb-pattern|firmware-pattern|display-audit|nzeus-modern ' | grep -v grep && {
    echo ANOTHER_SCREEN_PROGRAM_RUNNING_STOP; exit 1; }
grep -E "MemTotal|MemAvailable" /proc/meminfo
/tmp/nzeus-tools/nzeus-modern "$LIMIT" > $LOG 2>&1 &
echo $! > /tmp/nzeus-modern.pid
echo "NZEUS_MODERN_STARTED pid=$! seconds=$LIMIT uptime=$(cut -d' ' -f1 /proc/uptime)"
sleep 3
cat $LOG
