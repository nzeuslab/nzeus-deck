# Any modern NZEUS kernel. Reads the button serializer for 60 s while I
# press buttons in a fixed order (see README.md). key-scan changes ONLY
# GPIO L08/L09 (the serializer's own clock and data pins), checks no other pin
# changed, and puts L08/L09 back to output-high at the end.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-*) ;;
    *) echo REFUSED_NOT_MODERN_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/key-scan || { echo KEY_SCAN_NOT_UPLOADED; exit 1; }
uname -r
/tmp/nzeus-tools/key-scan 60
echo "KEY_SCAN_EXIT=$?"
dmesg | tail -n 3
