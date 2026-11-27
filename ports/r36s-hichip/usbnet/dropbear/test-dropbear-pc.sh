#!/bin/bash
# PC test of dropbearmulti-modern-mipsel and dropbear-start.sh under qemu-mipsel.
#  Part 1: the MIPS server + MIPS client log in to each other over 127.0.0.1
#          (key login, a command, a wrong key refused).
#  Part 2: dropbear-start.sh run by the console's exact boot BusyBox with only the
#          boot applets on PATH; console folders replaced by temp folders.
# qemu-user runs the MIPS code on the PC's kernel: it proves the program and the
# script, NOT the console kernel. (It does enforce read-only code pages, so a
# program that writes into its own code would crash here too.)
# Needs root (part 1 makes a 700 folder at / for authorized_keys), python3, qemu-mipsel.
#   bash ports/r36s-hichip/usbnet/dropbear/test-dropbear-pc.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
QEMU=$(command -v qemu-mipsel)
DBM=$HERE/dropbearmulti-modern-mipsel
T=$(mktemp -d /tmp/nzeus-dropbear-test.XXXXXX)
A=/nzeus-dbtest-$$            # authorized_keys folder: Dropbear checks every folder up to /
P1=$((20000 + $$ % 20000)); P2=$((P1 + 1))   # test ports on 127.0.0.1 only
export HOME=$T                 # the client must not touch the real ~/.ssh
cleanup() { pkill -f "$DBM dropbear" 2>/dev/null; rm -rf "$T" "$A"; }
trap cleanup EXIT
pass=0; fail=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); return 1; fi; }
db() { "$QEMU" "$DBM" "$@"; }

echo "== part 1: MIPS server <-> MIPS client"
ok "static, no interpreter" "! readelf -l '$DBM' 2>/dev/null | grep -q INTERP"
ok "version" "db dropbear -V 2>&1 | grep -q 'Dropbear v2026.94'"
db dropbearkey -t ed25519 -f "$T/host" > /dev/null 2>&1
db dropbearkey -t ed25519 -f "$T/client" > /dev/null 2>&1
db dropbearkey -t ed25519 -f "$T/stranger" > /dev/null 2>&1
mkdir -m 700 "$A"
db dropbearkey -y -f "$T/client" | grep '^ssh-ed25519 ' > "$A/authorized_keys"
chmod 600 "$A/authorized_keys"
ok "keys made" "test -s '$A/authorized_keys'"
db dropbear -F -E -j -k -r "$T/host" -p 127.0.0.1:$P1 -D "$A" -P "$T/pid" > "$T/server.log" 2>&1 &
server=$!
sleep 3
# -y -y: no host key check and nothing written to known_hosts (test only)
out=$(db dbclient -y -y -i "$T/client" -p $P1 root@127.0.0.1 'echo LOGIN_OK' 2>&1 < /dev/null)
ok "key login runs a command" "echo \"\$out\" | grep -q LOGIN_OK" || { echo "$out"; tail -5 "$T/server.log"; }
out=$(db dbclient -y -y -i "$T/stranger" -p $P1 root@127.0.0.1 'echo LOGIN_OK' 2>&1 < /dev/null)
ok "unknown key refused" "! echo \"\$out\" | grep -q LOGIN_OK"
ok "pid file written with -F" "test -s '$T/pid'"
kill "$server" 2>/dev/null; wait "$server" 2>/dev/null
rm -rf "$A"

echo "== part 2: dropbear-start.sh with the boot BusyBox"
BB=$T/busybox-boot
python3 - "$HERE/../../display/bootlog/build-usbfix/initramfs.cpio.gz" "$BB" <<'EOF'
import gzip, sys
d = gzip.decompress(open(sys.argv[1], 'rb').read()); pos = 0
while True:
    f = [int(d[pos+6+i*8:pos+14+i*8], 16) for i in range(13)]; pos += 110
    name = d[pos:pos+f[11]-1].decode(); pos = (pos + f[11] + 3) & ~3
    data = d[pos:pos+f[6]]; pos = (pos + f[6] + 3) & ~3
    if name == 'bin/busybox':
        open(sys.argv[2], 'wb').write(data); break
    assert name != 'TRAILER!!!'
EOF
chmod 755 "$BB"; mkdir -p "$T/bin" "$T/tools" "$T/state" "$T/fakebin"
for a in $("$QEMU" "$BB" --list); do
    printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$QEMU" "$BB" "$a" > "$T/bin/$a"
done
printf '#!/bin/sh\necho "2: usb0: <UP> mtu 1500"\necho "    inet 192.168.77.1/24 scope global usb0"\n' > "$T/bin/fakeip"
chmod 755 "$T/bin/"*
# the uploaded program, as the console would run it (here through qemu)
printf '#!/bin/sh\nexec %s %s "$@"\n' "$QEMU" "$DBM" > "$T/tools/dropbearmulti-modern"
chmod 700 "$T/tools/dropbearmulti-modern"
S=$T/dropbear-start.sh; cp "$HERE/dropbear-start.sh" "$S"
con() {
    env -i PATH="$T/bin" QEMU_UNAME=7.2.0-rc5-nzeus-gma-native-net+ NZ_TOOLS="$T/tools" \
        NZ_USBNET_STATE="$T/state" NZ_SSH_HOME="$T/home" NZ_ETC="$T/etc" NZ_BIN="$T/fakebin" NZ_IP=fakeip \
        NZ_SSH_LISTEN=127.0.0.1:$P2 "$QEMU" "$BB" sh "$@" 2>&1
}
ok "sh -n" "'$QEMU' '$BB' sh -n '$S'"
out=$(QEMU_UNAME=4.4.186 env -i PATH="$T/bin" "$QEMU" -r 4.4.186 "$BB" sh "$S" 2>&1)
ok "stock kernel refused" "echo \"\$out\" | grep -q REFUSED"
out=$(con "$S"); ok "no key uploaded: refused" "echo \"\$out\" | grep -q 'MISSING.*authorized_keys'"
echo 'ssh-rsa AAAAB3Nza... pc' > "$T/tools/authorized_keys"
out=$(con "$S"); ok "RSA-only key file refused" "echo \"\$out\" | grep -q 'no ssh-ed25519'"
db dropbearkey -y -f "$T/client" | grep '^ssh-ed25519 ' > "$T/tools/authorized_keys"
out=$(con "$S"); ok "USB network not up: refused" "echo \"\$out\" | grep -q 'USB network is not up'"
echo usb0 > "$T/state/ifname"
out=$(con "$S"); echo "$out"
ok "started" "echo \"\$out\" | grep -q 'DROPBEAR_RUNNING'"
ok "fingerprint shown" "echo \"\$out\" | grep -qi 'fingerprint'"
ok "listening" "(exec 3<>/dev/tcp/127.0.0.1/$P2) 2>/dev/null"
ok "home 700, key 600" "test \"\$(stat -c %a '$T/home')\$(stat -c %a '$T/home/.ssh')\$(stat -c %a '$T/home/.ssh/authorized_keys')\" = 700700600"
ok "passwd line" "grep -qx 'root:x:0:0:root:$T/home:/bin/sh' '$T/etc/passwd'"
ok "scp linked where Dropbear's root PATH looks (/bin)" "test -L '$T/fakebin/scp'"
ok "own copy kept" "cmp -s '$S' '$T/state/dropbear-start.sh'"
out=$(con "$S"); ok "second start: already running" "echo \"\$out\" | grep -q DROPBEAR_ALREADY_RUNNING"
out=$(con "$T/state/dropbear-start.sh" --stop); ok "stop" "echo \"\$out\" | grep -q DROPBEAR_STOPPED"
sleep 1
ok "port closed after stop" "! (exec 3<>/dev/tcp/127.0.0.1/$P2) 2>/dev/null"
echo 'root:x:0:0:root:/root:/bin/bash' > "$T/etc/passwd"
out=$(con "$S"); ok "foreign /etc/passwd refused" "echo \"\$out\" | grep -q 'is not ours'"
echo "RESULT: $pass passed, $fail failed"
test "$fail" = 0
