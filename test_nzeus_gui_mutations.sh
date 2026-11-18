#!/bin/bash
# Proves the NZEUS tests can actually catch mistakes: each mutant breaks one
# source file on purpose; the test run on the broken copy must report failures.
# Mutants are built and run in parallel. Each run gets its own private /tmp
# (unshare), so they can't disturb each other. The flags are build_nzeus_gui.sh's
# (gcc, -DNZ_TESTING, the same core: hw.c + hw_modern.c with fake files).
# Run `bash build_nzeus_gui.sh pc` first (it builds the LVGL objects used here).
NZ_BUILD=${NZ_BUILD:-$HOME/r36s/build}       # the same folder build_nzeus_gui.sh used
S=$NZ_BUILD/nzeus-gui/src
B=$NZ_BUILD/nzeus-gui/pc
L=~/r36s/src/lvgl
M=$NZ_BUILD/nzeus-gui/mutants
WIN="$(cd "$(dirname "$0")" && pwd)"
rm -rf $M && mkdir -p $M
. $S/sources.sh                                   # the list of .c files
ALL="$CONSOLE_FILES $UI_FILES"

# mutants.txt: one per line:  file @@ description @@ exact text @@ replacement
# (lines starting with # are comments). Python makes one folder per mutant.
n=$(python3 - "$WIN/mutants.txt" "$S" "$M" <<'PY'
import sys, os, shutil
spec, src, out = sys.argv[1:4]
n = 0
for line in open(spec, encoding="utf-8"):
    line = line.rstrip()
    if not line.strip() or line.lstrip().startswith("#"): continue
    f, desc, old, new = [p.strip().replace("<NL>", chr(10)) for p in line.split("@@")]
    n += 1
    d = os.path.join(out, str(n))
    shutil.copytree(src, d)
    open(os.path.join(d, "desc"), "w").write(desc)
    path = os.path.join(d, f)
    code = open(path).read()
    if old not in code:
        open(os.path.join(d, "result"), "w").write("MUTATION TARGET NOT FOUND")
        continue
    open(path, "w").write(code.replace(old, new, 1))
print(n)
PY
)
# at most JOBS mutants at a time (all at once overloads the PC: each run then
# gets so slow that the 300 s limit would count it as "caught" for nothing)
JOBS=${NZ_MUT_JOBS:-$(($(nproc) * 2))}
for i in $(seq 1 $n); do
    d=$M/$i
    [ -f $d/result ] && continue
    while [ $(jobs -rp | wc -l) -ge $JOBS ]; do wait -n; done
    (
        files=""; for f in $ALL; do files="$files $d/$f"; done
        if ! gcc -O1 -w -I$d -I$d/vterm/include -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE -DNZ_TESTING -DNZ_SDCARD='"/tmp/fake_sdcard"' "-DNZ_REPO=\"$WIN\"" \
                -o $d/ff $d/tests/fake_frontend.c $files $B/third/*.o $B/lvgl/*.o -lm 2>$d/cc.log; then
            echo "BUILD FAILED (fix the mutant)" > $d/result; exit
        fi
        # private /tmp + private shared memory; a hang (timeout) or a crash counts as caught
        # ("FAIL hang" / "FAIL crash" on a line of their own: the run may have stopped in the
        #  middle of a line, and a crash loses whatever it had not written out yet)
        c=$(unshare -r -m -i sh -c "mount -t tmpfs none /tmp && cd $d && { timeout 300 ./ff /tmp $d/tests/pics >out.txt 2>&1; rc=\$?; [ \$rc -eq 124 ] && printf '\nFAIL hang\n' >> out.txt; [ \$rc -gt 128 ] && printf '\nFAIL crash (signal %d)\n' \$((rc - 128)) >> out.txt; }; grep -c '^FAIL' out.txt")
        echo "$c failing checks" > $d/result
    ) &
done
wait

survived=0
for i in $(seq 1 $n); do
    d=$M/$i/
    r=$(cat $d/result 2>/dev/null)
    printf '%-48s -> %s\n' "$(cat $d/desc)" "$r"
    case "$r" in "0 failing"*|"MUTATION TARGET"*|"BUILD FAILED"*|"") survived=$((survived + 1));; esac
done
if [ $survived -eq 0 ]; then echo "ALL $n MUTANTS CAUGHT"; else echo "$survived MUTANT(S) SURVIVED OR MISSING"; exit 1; fi
