#!/bin/bash
# Tests the NZEUS Probe sketch on the PC, once per board type (Pico, STM32, a
# plain Arduino), with a pretend Arduino. Run in WSL:
#   bash test_probe.sh            run the checks
#   bash test_probe.sh mutants    deliberate mistakes must make checks FAIL
HERE=$(cd "$(dirname "$0")" && pwd)
INO=${INO:-$HERE/../firmware/nzeus_probe/nzeus_probe.ino}
T=$(mktemp -d /tmp/nzprobe.XXXXXX)
trap 'rm -rf "$T"' EXIT
QUIET=${QUIET:-0}

run_all() {     # $1 = the sketch to test; prints "== N passed, M failed" (all boards)
    local p=0 f=0
    mkdir -p "$T/fw/nzeus_probe" "$T/test"
    cp "$1" "$T/fw/nzeus_probe/nzeus_probe.ino"
    cp "$HERE/mock_arduino.h" "$HERE/Wire.h" "$HERE/test_probe.cpp" "$T/test/"
    sed -i 's#\.\./firmware/nzeus_probe/#../fw/nzeus_probe/#' "$T/test/test_probe.cpp"
    for board in "-DARDUINO_ARCH_RP2040" "-DARDUINO_ARCH_STM32" ""; do
        if ! g++ -std=c++17 -Wall -Wno-unused-function $board -I"$T/test" -o "$T/t" "$T/test/test_probe.cpp" 2> "$T/cc.log"; then
            [ "$QUIET" = 1 ] || { echo "FAIL  does not compile ($board)"; head -5 "$T/cc.log"; }
            f=$((f + 1)); continue
        fi
        local out; out=$("$T/t")
        [ "$QUIET" = 1 ] || echo "$out" | grep -v '^=='
        p=$((p + $(echo "$out" | grep -c '^PASS'))); f=$((f + $(echo "$out" | grep -c '^FAIL')))
    done
    echo "== $p passed, $f failed"
    [ "$f" = 0 ]
}

run_all "$INO"; RESULT=$?
if [ "$1" = mutants ]; then
    echo "== mutants"
    mutant() {
        local m=$T/mutant.ino
        sed "$2" "$INO" > "$m"
        if cmp -s "$m" "$INO"; then echo "BAD MUTANT: $1"; RESULT=1; return; fi
        local out; out=$(QUIET=1 run_all "$m" | tail -1)
        case "$out" in *", 0 failed") echo "NOT CAUGHT  $1   ($out)"; RESULT=1 ;; *) echo "caught      $1   ($out)" ;; esac
    }
    mutant "voltage not scaled to mV"          's/return raw \* VREF_MV \/ ((1L << ADC_BITS) - 1);/return raw;/'
    mutant "channel 0 accepted"                's/else if (a < 1 || a > N_ADC) snprintf(reply, n, "ERR channel 1..%d", N_ADC);\r\{0,1\}$/else if (a > N_ADC) snprintf(reply, n, "ERR channel 1..%d", N_ADC);/'
    mutant "logic levels inverted"             "s/? '1' : '0'/? '0' : '1'/"
    mutant "scope does not wait between samples" 's/while ((long)(micros() - (t0 + (unsigned long)(i \* c))) < 0) { }/;/'
    mutant "scope allows 1000 samples"         's/#define MAX_SAMPLES 200/#define MAX_SAMPLES 1000/'
    mutant "duty 100 % accepted"               's/b < 1 || b > 99/b < 1 || b > 100/'
    mutant "I2C scan skips the found chips"    's/if (Wire.endTransmission() == 0)/if (Wire.endTransmission() != 0)/'
    mutant "long lines not refused"            's/too_long = 1; /too_long = 0; /'
fi
exit $RESULT
