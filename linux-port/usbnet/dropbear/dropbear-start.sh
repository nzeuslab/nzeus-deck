#!/bin/sh
# dropbear-start.sh - start the SSH server on the MODERN Linux console, listening
# ONLY on the USB network address 192.168.77.1 port 22 (usbnet-up.sh first).
#
# Before (from the PC, over the rescue serial port; uploads go to RAM only):
#   python linux-port\diagnostics\rescue-upload.py linux-port\usbnet\dropbear\dropbearmulti-modern-mipsel --name dropbearmulti-modern
#   python linux-port\diagnostics\rescue-upload.py %USERPROFILE%\.ssh\id_ed25519.pub --name authorized_keys
#   (optional, same server identity after every reboot - see README:)
#   python linux-port\diagnostics\rescue-upload.py <your saved host key> --name ssh_host_ed25519
# Then:
#   python linux-port\diagnostics\rescue-command.py linux-port\usbnet\dropbear\dropbear-start.sh
#   sh /tmp/nzeus-usbnet/dropbear-start.sh --stop        (stops it; the copy is kept)
#
# What it sets up (all in RAM, gone after power-off; the SD card is never touched):
#   /tmp/nzeus-ssh/                 root's home for SSH (mode 700) - Dropbear checks
#     .ssh/authorized_keys           the folders up to the home folder, so /tmp
#     host_ed25519                   itself (mode 1777) is never checked
#   /etc/passwd                      one line for root (the RAM system has no /etc);
#                                    created only if missing
# Logins: public key only (no passwords are compiled in). Only ed25519/ecdsa keys.
# The host key fingerprint is printed here, over the trusted serial cable: compare
# it with what ssh shows the first time.
# Boot BusyBox only: no '[' (use test), no rm/tr/timeout/printf.
# NZ_* variables are for the PC test only.

TOOLS=${NZ_TOOLS:-/tmp/nzeus-tools}
NETSTATE=${NZ_USBNET_STATE:-/tmp/nzeus-usbnet}
H=${NZ_SSH_HOME:-/tmp/nzeus-ssh}
ETC=${NZ_ETC:-/etc}
BIN=${NZ_BIN:-/bin}
IP=${NZ_IP:-ip}
ADDR=192.168.77.1
LISTEN=${NZ_SSH_LISTEN:-$ADDR:22}
DB=$TOOLS/dropbearmulti-modern
PID=$H/dropbear.pid

case "$(uname -r)" in
    *-nzeus-gma-native*) ;;
    *) echo 'REFUSED: only for the NZEUS modern kernels (gma-native*).'; exit 1 ;;
esac

# running: true only if the pid file holds a real, live process number.
# (In BusyBox ash "kill -0 ''" / "kill ''" mean process 0 = our own process
# group, so an empty pid file must never reach kill.)
running() {
    pid=$(cat "$PID" 2>/dev/null)
    case "$pid" in ''|0|*[!0-9]*) return 1 ;; esac
    kill -0 "$pid" 2>/dev/null
}

if test "$1" = --stop; then
    if running; then
        kill "$pid" && echo DROPBEAR_STOPPED
    else
        echo DROPBEAR_NOT_RUNNING
    fi
    exit 0
fi

running && { echo "DROPBEAR_ALREADY_RUNNING pid=$pid on $LISTEN"; exit 0; }
test -x "$DB" || { echo "MISSING $DB: upload dropbearmulti-modern-mipsel as 'dropbearmulti-modern'"; exit 1; }
test -f "$TOOLS/authorized_keys" || { echo "MISSING $TOOLS/authorized_keys: upload your PC public key (.pub)"; exit 1; }
if ! grep -q -E '^(ssh-ed25519|ecdsa-sha2-nistp(256|384|521)) ' "$TOOLS/authorized_keys"; then
    echo 'REFUSED: authorized_keys has no ssh-ed25519 / ecdsa key line (RSA is not built in).'
    exit 1
fi
# Listen only on the USB network: it must be up with the console address.
ifname=$(cat "$NETSTATE/ifname" 2>/dev/null)
test -n "$ifname" || { echo 'REFUSED: USB network is not up (run usbnet-up.sh first).'; exit 1; }
$IP addr show dev "$ifname" 2>/dev/null | grep -q "inet $ADDR/" || {
    echo "REFUSED: $ifname does not have $ADDR (usbnet-up.sh --status)."; exit 1; }

# Our own copy, so --stop works after rescue-command.py reused its temp file.
mkdir -p "$NETSTATE"
test "$0" = "$NETSTATE/dropbear-start.sh" || cp "$0" "$NETSTATE/dropbear-start.sh"

mkdir -p "$H/.ssh" "$H/bin" || exit 1
chmod 700 "$H" "$H/.ssh" || exit 1
cp "$TOOLS/authorized_keys" "$H/.ssh/authorized_keys" && chmod 600 "$H/.ssh/authorized_keys" || exit 1
# 'scp' for the server side of scp (use 'scp -O' from new OpenSSH: no sftp here).
# Dropbear always sets a root login's PATH to /usr/sbin:/usr/bin:/sbin:/bin
# (seen on the console 2026-10-10: "sh: scp: not found"), so the link must be
# in /bin - the RAM system's /bin (initramfs, RAM only). An existing scp that
# is not ours is left alone.
test -L "$H/bin/scp" || ln -s "$DB" "$H/bin/scp"
if test ! -e "$BIN/scp"; then
    ln -s "$DB" "$BIN/scp" && echo "scp: linked $BIN/scp (RAM)"
fi

if test ! -f "$ETC/passwd"; then
    mkdir -p "$ETC" && echo "root:x:0:0:root:$H:/bin/sh" > "$ETC/passwd" || exit 1
    echo "made $ETC/passwd (RAM): root home = $H"
elif ! grep -q "^root:x:0:0:root:$H:/bin/sh\$" "$ETC/passwd"; then
    echo "REFUSED: $ETC/passwd exists but is not ours; root's home must be $H."
    exit 1
fi

if test -f "$TOOLS/ssh_host_ed25519"; then
    cp "$TOOLS/ssh_host_ed25519" "$H/host_ed25519" || exit 1
    echo 'host key: the uploaded one (same identity as before)'
elif test ! -f "$H/host_ed25519"; then
    "$DB" dropbearkey -t ed25519 -f "$H/host_ed25519" > /dev/null 2>&1 || { echo 'could not make a host key'; exit 1; }
    echo 'host key: NEW for this boot (RAM only) - ssh will warn that it changed'
fi
chmod 600 "$H/host_ed25519"
echo "host key fingerprint (compare on first login):"
"$DB" dropbearkey -y -f "$H/host_ed25519" 2>/dev/null | grep -i fingerprint

# -F foreground (we detach it ourselves with setsid), -E log to the log file,
# -j -k no port forwarding, -e pass PATH below to logins (so 'scp' is found).
PATH=$H/bin:/bin:$TOOLS:$PATH
export PATH
setsid "$DB" dropbear -F -E -e -j -k -r "$H/host_ed25519" -p "$LISTEN" -P "$PID" \
    > "$H/dropbear.log" 2>&1 < /dev/null &
sleep 2
if running; then
    echo "DROPBEAR_RUNNING pid=$pid on $LISTEN"
    echo "From the PC (PowerShell):  ssh -o UserKnownHostsFile=\$env:USERPROFILE\\.ssh\\nzeus_known_hosts root@$ADDR"
else
    echo 'DROPBEAR_FAILED - its log:'
    tail -n 20 "$H/dropbear.log"
    exit 1
fi
