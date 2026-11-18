# NZEUS SD Linux: short warnings at login (SSH). The USB shell shows the same
# line in its banner (nzeus-status --short).
case "$(cat /run/nzroot-state 2>/dev/null)" in
    ro*) echo "WARNING: the SD root is READ-ONLY: $(cat /run/nzroot-state). See /etc/motd." ;;
esac
grep -q ' /data ' /proc/mounts || echo "Note: /data is not mounted (nzeus-status)."
