#!/bin/bash
# PC test for the console's side of the NZEUS Probe (apps/nzeus-gui/probe.c).
#   bash test_probe_console.sh            run the checks
#   bash test_probe_console.sh mutants    deliberate mistakes must make checks FAIL
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HERE/../../nzeus-gui/probe.c}
INC=$HERE/../../nzeus-gui
T=$(mktemp -d /tmp/nzprobec.XXXXXX)
trap 'rm -rf "$T"' EXIT

run_test() {    # $1 = probe.c to test
    if ! gcc -std=c99 -Wall -Wextra -I"$INC" -o "$T/t" "$HERE/test_probe_console.c" "$1" 2> "$T/cc.log"; then
        echo "FAIL  does not compile"; head -5 "$T/cc.log"; echo "== 0 passed, 1 failed"; return 1
    fi
    "$T/t"
}

run_test "$SRC"; RESULT=$?
if [ "$1" = mutants ]; then
    echo "== mutants"
    mutant() {
        local m=$T/mutant.c
        sed "$2" "$SRC" > "$m"
        if cmp -s "$m" "$SRC"; then echo "BAD MUTANT: $1"; RESULT=1; return; fi
        local out; out=$(run_test "$m" | tail -1)
        case "$out" in *", 0 failed") echo "NOT CAUGHT  $1   ($out)"; RESULT=1 ;; *) echo "caught      $1   ($out)" ;; esac
    }
    mutant "any protocol version accepted"     's/ || ver != 1) return 0;/) return 0;/'
    mutant "negative voltages accepted"        "s/if (!\*\*p || \*\*p == '-' || \*\*p == '+') return 0;/if (!**p) return 0;/"
    mutant "extra numbers after V accepted"    's/!num(&p, \&v, 10) || !at_end(p)) return 0;/!num(\&p, \&v, 10)) return 0;/'
    mutant "logic: any character is a 0"       "s/if (\*p != '0' \&\& \*p != '1') return 0;/;/"
    mutant "scope: extra values accepted"      's/    if (!at_end(p)) return 0;                        \/\* more values/    \/\* more values/'
    mutant "I2C: impossible addresses"         's/ || a < 0x08 || a > 0x77) return -1;/) return -1;/'
    mutant "lines: CR kept in the line"        "s/if (c == '\\\\r') continue; /if (0) continue; /"
fi
exit $RESULT
