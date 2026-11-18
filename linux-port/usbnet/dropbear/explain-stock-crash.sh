#!/bin/bash
# explain-stock-crash.sh - read-only PC look at WHY the stock dropbearmulti crashed
# on the modern kernel (2026-10-10, sd-run-test.sh):
#     invalid write access to 0048a0e8, epc 0048a0f0  in dropbearmulti[8a0f0,400000+f0000]
# = the instruction at 0x48a0f0 stored to 0x48a0e8, inside the program's FIRST
# load segment (0x400000, 0xf0000 bytes), which the kernel mapped without write
# permission. This prints the program headers, the section and function that hold
# both addresses, and the code around them, so the cause can be named exactly.
# Nothing is run; the binary is only read.
#   bash linux-port/usbnet/dropbear/explain-stock-crash.sh apps/dropbear/mips/dropbearmulti
# Needs: binutils-mipsel-linux-gnu (mipsel-linux-gnu-readelf/objdump/nm).
set -euo pipefail
F=${1:-apps/dropbear/mips/dropbearmulti}
STORE=${STORE:-0x48a0e8}     # (overridable only to try the script on another binary)
EPC=${EPC:-0x48a0f0}
P=mipsel-linux-gnu-
test -f "$F" || { echo "no such file: $F"; exit 1; }
echo "== file"
ls -l "$F"; sha256sum "$F"
${P}readelf -h "$F" | grep -E 'Type|Entry|Flags'
${P}readelf -p .comment "$F" 2>/dev/null | grep -i gcc || true
echo "== program headers (look at the LOAD lines: flags R E / RW / RWE)"
${P}readelf -lW "$F"
echo "== which section holds the store address and the instruction"
${P}readelf -SW "$F" | python3 -c '
import sys
store, epc = int(sys.argv[1], 16), int(sys.argv[2], 16)
for line in sys.stdin:
    parts = line.replace("[ ", "[").split()
    if len(parts) > 6 and parts[0].startswith("[") and parts[0] != "[Nr]":
        try:
            name, addr, size = parts[1], int(parts[3], 16), int(parts[5], 16)
        except ValueError:
            continue
        flags = parts[7] if len(parts) > 7 else ""
        for label, a in (("store", store), ("instruction", epc)):
            if addr <= a < addr + size:
                print("%-11s 0x%x is in %s (flags %s)" % (label, a, name, flags))
' "$STORE" "$EPC"
echo "== function containing the instruction (if the file still has symbols)"
${P}nm -n "$F" 2>/dev/null | python3 -c '
import sys
epc, last = int(sys.argv[1], 16), None
for line in sys.stdin:
    v = line.split()
    if len(v) == 3 and int(v[0], 16) <= epc:
        last = line.strip()
print(last or "(stripped: no symbols)")
' "$EPC"
echo "== code around the instruction (the faulting store is at or right after 48a0f0)"
${P}objdump -d --start-address=$((EPC - 0x40)) --stop-address=$((EPC + 0x20)) "$F" | tail -n +7
echo
echo "Reading the result:"
echo " - store address inside .text/.init/.rodata  -> the program writes into its own code"
echo "   (self-modifying code or a packer); the fix is a normal build (build-dropbear-modern.sh)."
echo " - first LOAD flagged RWE and the old kernel ran it -> the new kernel maps it differently;"
echo "   again a normal two-segment build avoids it."
echo " - store into .data/.got/.sdata but that section lies inside the R E segment -> linker"
echo "   layout problem of the old SDK toolchain."
