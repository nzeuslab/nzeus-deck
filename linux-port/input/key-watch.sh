# Any modern NZEUS kernel. READ-ONLY: watch the GPIO input pins for 60 s while
# I press buttons in a fixed order (see README.md). key-watch maps the
# GPIO register page PROT_READ from /dev/mem opened O_RDONLY; it writes nothing.
case "$(uname -r)" in
    7.2.0-rc5-nzeus-*) ;;
    *) echo REFUSED_NOT_MODERN_KERNEL; exit 1 ;;
esac
test -x /tmp/nzeus-tools/key-watch || { echo KEY_WATCH_NOT_UPLOADED; exit 1; }
uname -r
/tmp/nzeus-tools/key-watch 60
echo "KEY_WATCH_EXIT=$?"
dmesg | tail -n 3
