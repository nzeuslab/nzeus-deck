# gma-native-power only. Switches on 32 MiB of compressed swap in RAM (zram)
# and shows memory before and after. RAM only; nothing touches the SD card.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_POWER_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/nz-zram || { echo NZ_ZRAM_NOT_UPLOADED; exit 1; }
grep -E 'MemTotal|MemFree|MemAvailable|SwapTotal' /proc/meminfo
grep -q zram0 /proc/swaps && { echo ZRAM_ALREADY_ON; cat /proc/swaps; exit 0; }
/tmp/nzeus-tools/nz-zram 32
echo "NZ_ZRAM_EXIT=$?"
cat /proc/swaps
grep -E 'SwapTotal|SwapFree' /proc/meminfo
cat /sys/block/zram0/comp_algorithm
