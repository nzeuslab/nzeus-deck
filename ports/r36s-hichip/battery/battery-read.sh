# Any modern NZEUS kernel. READ-ONLY battery and charger reading for 60 s
# (one line per second). bat-read maps the ADC and system register pages
# PROT_READ from /dev/mem opened O_RDONLY; it writes nothing.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-*) ;;
    *) echo REFUSED_NOT_MODERN_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/bat-read || { echo BAT_READ_NOT_UPLOADED; exit 1; }
uname -r
echo "UPTIME $(cut -d' ' -f1 /proc/uptime)"
/tmp/nzeus-tools/bat-read 60
echo "BAT_READ_EXIT=$?"
