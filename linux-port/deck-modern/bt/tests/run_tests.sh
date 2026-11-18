#!/bin/bash
# PC tests of nzeus-bt (called by ../build.sh after it made the test builds):
#   1. unit tests (x86 with memory checkers, MIPS under qemu-mipsel)
#   2. flow tests: every nzeus-bt command against the fake kernel (x86 + MIPS)
#   3. C mutants (deliberate mistakes the tests must catch; ~40 minutes),
#      only with NZBT_MUTANTS=1
# No console, no dongle: the real Bluetooth is checked on the console only.
HERE="$(cd "$(dirname "$0")" && pwd)"
B=${NZBT_BUILD:-~/r36s/build/nzeus-bt-modern}
fail=0
echo "--- unit tests"
"$B/unit-x86" | tail -1; test "${PIPESTATUS[0]}" = 0 || fail=1
qemu-mipsel "$B/unit-mips" | tail -1; test "${PIPESTATUS[0]}" = 0 || fail=1
echo "--- flow tests (x86 build with address/undefined-behaviour checkers)"
python3 "$HERE/test_flows.py" "$B/nzeus-bt-fake-x86" > "$B/flows-x86.log" 2>&1 || fail=1
grep -E "FAIL|flow tests:" "$B/flows-x86.log"
echo "--- flow tests (MIPS build under qemu-mipsel)"
python3 "$HERE/test_flows.py" "$B/nzeus-bt-fake-mips" qemu-mipsel > "$B/flows-mips.log" 2>&1 || fail=1
grep -E "FAIL|flow tests:" "$B/flows-mips.log"
if test "${NZBT_MUTANTS:-}" = 1; then
    echo "--- C mutants"
    bash "$HERE/mutants.sh" || fail=1
fi
if test $fail = 0; then echo "ALL NZEUS-BT TESTS PASSED"; else echo "SOME TESTS FAILED (logs in $B)"; exit 1; fi
