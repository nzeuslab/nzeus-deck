# NZEUS SD root: short status line at login.
if test -r /run/nzroot-fsck-rc; then
    case "$(cat /run/nzroot-fsck-rc)" in
        0|1) ;;
        *) echo "WARNING: the SD root is READ-ONLY (e2fsck rc $(cat /run/nzroot-fsck-rc)). See PLAN.md." ;;
    esac
fi
grep -q ' /data ' /proc/mounts || echo "Note: /data is not mounted."
export PS1='\u@\h:\w\$ '
