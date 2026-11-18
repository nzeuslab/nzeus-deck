# gma-native-power only. Brightness through the standard Linux backlight
# device (the driver sends the stock 4-byte level to the AVP's /dev/backlight).
# Steps: 153 (factory default) -> 60 (dim) -> 255 (full) -> back to the level
# it had before, 5 s each, with the PWM registers read after each step.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-gma-native-power*|7.2.0-rc5-nzeus-gma-native-pure*) ;;
    *) echo REFUSED_NOT_POWER_KERNEL; exit 1 ;;
esac
B=/sys/class/backlight/r36s-avp
test -d $B || { echo NO_BACKLIGHT_DEVICE; exit 1; }
start=$(cat $B/brightness)
echo "max=$(cat $B/max_brightness) start=$start type=$(cat $B/type)"
dmesg | grep 'backlight PWM0' | tail -n 1
for level in 153 60 255 $start; do
    echo "== set $level"
    echo $level > $B/brightness
    dmesg | grep BACKLIGHT | tail -n 1
    test -x /tmp/nzeus-tools/bl-read && /tmp/nzeus-tools/bl-read | grep -E '^PWM0 '
    sleep 5
done
echo "BRIGHTNESS_TEST_DONE now=$(cat $B/brightness)"
