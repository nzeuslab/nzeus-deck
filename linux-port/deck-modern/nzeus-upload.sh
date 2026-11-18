#!/bin/sh
# nzeus-upload.sh - upload a ready-made firmware file to a board, from the console.
# THE SD DEBIAN VERSION (linux-port/deck-modern) of apps/upload/nzeus-upload.sh:
# same arguments, same output lines, same exit codes. What differs:
#   - the tools are Debian's own /usr/bin/avrdude (+ /etc/avrdude.conf) and
#     /usr/bin/stm32flash (packages avrdude + stm32flash in the system image),
#     with a plain message when one is not installed
#   - CH340/CH341 adapters are fine for STM32: the new kernel's ch341 driver
#     sends parity (mainline Linux since 4.10), so the stock check for the
#     NZEUS ch341.ko back-port is gone
#
#   nzeus-upload.sh BOARD PORT FILE
#     BOARD  uno       Arduino Uno (and most Uno clones)          avrdude, 115200 baud
#            nano      Arduino Nano, NEW bootloader (2018+)       avrdude, 115200 baud
#            nano-old  Arduino Nano, OLD bootloader (most clones) avrdude,  57600 baud
#            mega      Arduino Mega 2560                          avrdude, 115200 baud
#            stm32     STM32 chip, built-in serial bootloader     stm32flash
#     PORT   the serial port, e.g. /dev/ttyACM0 or /dev/ttyUSB0
#     FILE   .hex from the Arduino IDE (Sketch > Export Compiled Binary),
#            or for stm32 a .bin or .hex
#
# What it prints (one message per line, so the GUI can show them as they come):
#   STEP <text>               what it is doing now
#   PROGRESS write <0-100>    how far the writing is, in percent
#   PROGRESS verify <0-100>   how far the checking (reading back) is
#   any other line            a message from avrdude / stm32flash, as it is
#   last line, always one of:
#   Upload OK
#   Upload failed: <reason in plain words>
# Exit code: 0 = OK, 1 = failed.
#
# Settings (environment variables; the defaults are right for the console):
#   AVRDUDE       the avrdude program      (default /usr/bin/avrdude)
#   AVRDUDE_CONF  avrdude's parts list     (default /etc/avrdude.conf)
#   STM32FLASH    the stm32flash program   (default /usr/bin/stm32flash)
#   STM32_BAUD    speed for stm32          (default 115200; try 57600 with long wires)
#   SYSFS_ROOT    where sysfs is           (default /sys; only the PC test changes it)
#   STM32_SERIAL_MODE  PC TEST ONLY (default 8e1, what every STM32 bootloader needs)
# Written for dash / busybox ash (no bash-only features).

AVRDUDE=${AVRDUDE:-/usr/bin/avrdude}
AVRDUDE_CONF=${AVRDUDE_CONF:-/etc/avrdude.conf}
STM32FLASH=${STM32FLASH:-/usr/bin/stm32flash}
STM32_BAUD=${STM32_BAUD:-115200}
STM32_SERIAL_MODE=${STM32_SERIAL_MODE:-8e1}
SYSFS_ROOT=${SYSFS_ROOT:-/sys}

fail() { echo "Upload failed: $*"; exit 1; }

[ $# = 3 ] || fail "use: nzeus-upload.sh BOARD PORT FILE (BOARD = uno, nano, nano-old, mega or stm32)"
BOARD=$1
PORT=$2
FILE=$3

# ---- which tool, chip, protocol and speed for each board ----
# MAX = the biggest program the board takes: the flash minus the bootloader
# (same numbers as "upload.maximum_size" in the Arduino IDE's boards.txt).
case $BOARD in
    uno)      TOOL=avrdude; PART=atmega328p; PROG=arduino; BAUD=115200; MAX=32256 ;;
    nano)     TOOL=avrdude; PART=atmega328p; PROG=arduino; BAUD=115200; MAX=32256 ;;
    nano-old) TOOL=avrdude; PART=atmega328p; PROG=arduino; BAUD=57600;  MAX=30720 ;;
    mega)     TOOL=avrdude; PART=atmega2560; PROG=wiring;  BAUD=115200; MAX=253952 ;;
    stm32)    TOOL=stm32flash; BAUD=$STM32_BAUD ;;
    *)        fail "unknown board '$BOARD' (use uno, nano, nano-old, mega or stm32)" ;;
esac

# ---- check everything BEFORE touching the board ----
DRV=""        # the kernel driver of the port (found for stm32 below)
echo "STEP Checking the file and the port"
[ -e "$FILE" ] || fail "file not found: $FILE"
[ -f "$FILE" ] || fail "not a file: $FILE"
[ -s "$FILE" ] || fail "the file is empty: $FILE"
[ -e "$PORT" ] || fail "no port $PORT - is the board plugged in? (wait a few seconds after plugging in)"
[ -c "$PORT" ] || fail "$PORT is not a serial port"
first=$(head -c 4 "$FILE" | tr -d '\000')     # first bytes say what kind of file it is
case $first in
    ?ELF*) fail "this is an .elf file - export a .hex (Arduino: Sketch > Export Compiled Binary)" ;;
esac
case $first in :*) KIND=hex ;; *) KIND=bin ;; esac
case $TOOL in
    avrdude)
        case $FILE in
            *with_bootloader*) fail "this is the .with_bootloader.hex file - copy the OTHER .hex (without with_bootloader in its name)" ;;
        esac
        [ $KIND = hex ] || fail "not an Intel HEX file: $FILE (Arduino boards need the .hex from Sketch > Export Compiled Binary)"
        # Too big? avrdude itself would write into the bootloader's space. An Intel
        # HEX line is ":LLAAAATT<data>CC" (LL = byte count, AAAA = address, TT = type;
        # types 02/04 move the base address). END = highest address used + 1.
        END=$(awk '
            function hex(s,   i, v) { v = 0; s = toupper(s)
                for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789ABCDEF", substr(s, i, 1)) - 1
                return v }
            /^:/ { n = hex(substr($0, 2, 2)); a = hex(substr($0, 4, 4)); t = substr($0, 8, 2)
                   if (t == "00" && n > 0 && base + a + n > end) end = base + a + n
                   else if (t == "02") base = hex(substr($0, 10, 4)) * 16
                   else if (t == "04") base = hex(substr($0, 10, 4)) * 65536 }
            END { print end + 0 }' "$FILE")
        [ "$END" -le $MAX ] || fail "the firmware is too big for this board: it reaches $END bytes, the most is $MAX"
        [ -f "$AVRDUDE" ] || fail "avrdude is not installed on this system ($AVRDUDE) - it comes with the system image (Debian package avrdude)"
        [ -f "$AVRDUDE_CONF" ] || fail "avrdude.conf is missing ($AVRDUDE_CONF) - the Debian package avrdude is broken, install it again"
        ;;
    stm32flash)
        if [ $KIND = bin ]; then
            case $FILE in *.bin|*.BIN) ;; *) fail "STM32 needs a .bin or .hex file: $FILE" ;; esac
        fi
        [ -f "$STM32FLASH" ] || fail "stm32flash is not installed on this system ($STM32FLASH) - it comes with the system image (Debian package stm32flash)"
        # The STM32 bootloader needs EVEN PARITY (8E1). Every adapter driver
        # of the new kernel sends it: cp210x, ftdi_sio, pl2303, cdc-acm and
        # ch341 (CH340/CH341: mainline has parity since Linux 4.10; only the
        # stock 4.4 driver needed the NZEUS back-port). The driver is still
        # looked up, for the note about very old CH341 chips further down.
        # Which driver owns the port: /sys/class/tty/ttyUSB0/device/driver -> ch341-uart
        drv=$(readlink "$SYSFS_ROOT/class/tty/${PORT##*/}/device/driver" 2>/dev/null)
        DRV=${drv##*/}
        ;;
esac

# ---- run the tool; its output goes through a filter that makes PROGRESS lines ----
# The tool's text is also kept in a log file, to find the reason if it fails.
TMP=$(mktemp -d /tmp/nzeus-upload.XXXXXX) || fail "cannot make a temporary folder in /tmp"
trap 'rm -rf "$TMP"' EXIT
LOG=$TMP/tool.log

# Filter: avrdude draws its progress bar as "Writing | ####...# | 100% 0.4s"
# (one # per 2%), stm32flash as "\rWrote address 0x08000100 (12.50%)".
# tr turns every # and \r into a line break, so awk sees each step at once and
# prints "PROGRESS write 40" etc. fflush() = send each line out immediately
# (busybox awk already does that; the fflush() keeps it true with other awks).
progress_filter() {
    tr '\r#' '\n\n' | awk '
        /^(Writing|Reading) \| ?$/ { ph = ($1 == "Writing") ? "write" : "verify"; n = 0
                                     print "PROGRESS " ph " 0"; fflush(); next }
        ph != "" && $0 == ""       { n++; if (n % 5 == 0) { print "PROGRESS " ph " " n * 2; fflush() }; next }
        ph != "" && /^ *\| *100%/  { print "PROGRESS " ph " 100"; fflush(); ph = ""; next }
        /(Wrote|Read) .*address 0x[0-9a-fA-F]+ \([0-9.]+%\)/ {
            ph = ($1 == "Read") ? "verify" : "write"
            if (match($0, /\([0-9.]+%\)/)) {
                pc = int(substr($0, RSTART + 1, RLENGTH - 3))
                if (pc != last[ph]) { print "PROGRESS " ph " " pc; fflush(); last[ph] = pc }
            }
            ph = ""; next
        }
        $0 == "" { next }
        { print; fflush() }'
}

if [ $TOOL = avrdude ]; then
    echo "STEP Uploading to $BOARD ($PART) on $PORT at $BAUD baud - the board resets now"
    # -D: don't erase the whole chip first (the bootloader erases page by page;
    #     this is what the Arduino IDE does too). Verifying after writing is on.
    { "$AVRDUDE" -C "$AVRDUDE_CONF" -p $PART -c $PROG -P "$PORT" -b $BAUD -D \
          -U "flash:w:$FILE:i" 2>&1; echo $? > "$TMP/rc"; } | tee "$LOG" | progress_filter
else
    echo "STEP Uploading to STM32 on $PORT at $BAUD baud (BOOT0 must be high, then press reset)"
    # -w write the file, -v check every block after writing it, -g 0x0: start
    # the new program when done (address 0 = the start of flash).
    { "$STM32FLASH" -b $BAUD -m $STM32_SERIAL_MODE -w "$FILE" -v -g 0x0 "$PORT" 2>&1; echo $? > "$TMP/rc"; } \
        | tee "$LOG" | progress_filter
fi
rc=$(cat "$TMP/rc" 2>/dev/null)
[ "$rc" = 0 ] && { echo "Upload OK"; exit 0; }

# ---- it failed: say why, in plain words ----
has() { grep -qiE "$1" "$LOG"; }
# (avrdude also says "unable to open port" after a board that doesn't answer,
#  so only the messages of a real open failure count here)
if has "can't open device|Failed to open port|No such file or directory|Permission denied|Device or resource busy"; then
    fail "could not open $PORT (unplugged? or another program is using it)"
elif [ $TOOL = avrdude ] && has "not in sync|getsync|not responding|programmer is not responding|timeout"; then
    case $BOARD in
        nano)     fail "the board did not answer - for an older Nano or a clone, choose nano-old" ;;
        nano-old) fail "the board did not answer - for a newer Nano, choose nano; also check the cable" ;;
        *)        fail "the board did not answer - check the board type and the cable" ;;
    esac
elif has "expected signature|double check chip"; then      # (every run prints "Device signature =")
    fail "wrong chip on the board - is it really a $BOARD?"
elif has "verification|mismatch|Failed to verify"; then
    fail "the check after writing found different bytes (bad cable or noisy power?) - try again"
elif has "Failed to init device|Failed to get ACK|Init failed"; then
    # very old CH341 chips (version below 0x30) can't switch parity on; the
    # driver says so once, when the adapter is plugged in
    case $DRV in
        ch341*) echo "Note: a very old CH340/CH341 chip can't send parity - if 'dmesg | grep ch341' says \"can't change parity\", use another adapter" ;;
    esac
    fail "the STM32 did not answer - set BOOT0 high, press reset, and check TX/RX are crossed"
elif has "Failed to write memory|Failed to read memory"; then
    # (when stm32flash's check after a block fails, it writes the block again,
    #  which the chip refuses - so a bad check also ends up here)
    fail "writing to the STM32 failed (bad cable or noisy power?) - try again"
elif has "out of range|exceeds|too large|address .* out of|beyond"; then
    fail "the firmware is too big for this chip"
elif [ -z "$rc" ]; then
    fail "the upload program did not run"
fi
# anything else: the last line the tool printed that looks like an error
why=$(grep -iE "error|fail" "$LOG" | tail -1 | sed 's/^avrdude[^:]*: *//')
[ -n "$why" ] || why=$(grep . "$LOG" | tail -1)
fail "${why:-unknown error (exit code $rc)}"
