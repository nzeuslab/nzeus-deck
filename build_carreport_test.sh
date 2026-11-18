#!/bin/bash
# Builds and tests the car health report PDF maker (apps/nzeus-gui/carreport*.c).
#   bash build_carreport_test.sh          everything: checks, PDFs + pictures, MIPS build, mutants
#   bash build_carreport_test.sh quick    only the checks (no pictures, no MIPS, no mutants)
# Run it from Windows like this (WSL):
#   cd C:\path\to\nzeus\R36S && wsl -d Ubuntu -- bash build_carreport_test.sh
#
# It uses its OWN build folder, ~/r36s/build/carreport-test, with its own copy
# of the sources and its own LVGL objects (built with a copy of lv_conf.h), so
# it never disturbs the main NZEUS build.
# Results:
#   reports/car/*.pdf          the English + Arabic sample reports (and long / demo / sick ones)
#   reports/car/png/*.png      every page as a picture, to compare with the originals
# Needs (WSL): gcc, python3, qpdf + poppler-utils (Ubuntu packages, for checking the PDFs),
# Windows Python with PyMuPDF (pixel checks).
set -e
MODE=${1:-full}
WIN="$(cd "$(dirname "$0")" && pwd)"
L=~/r36s/src/lvgl
B=~/r36s/build/carreport-test
S=$B/src
PYW="/mnt/c/Program Files/Python313/python.exe"
mkdir -p $S $B/lvgl $B/obj $B/mips $B/mut
# a copy of the sources: the space in "nzeus lab" breaks compiler include paths
rsync -a --delete --exclude build "$WIN/apps/nzeus-gui/" $S/

CC=gcc
INC="-I$S -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"
CFLAGS="-O2 -g"

# 1) LVGL with our copy of lv_conf.h (only files that changed are recompiled)
compile_lvgl() {
    f=$1
    o=$B/lvgl/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$f" ] && [ "$o" -nt "$S/lv_conf.h" ] && return 0
    $CC -O2 -w $INC -c "$f" -o "$o"
}
export -f compile_lvgl; export CC INC B L S
find $L/src -name '*.c' | xargs -P 32 -n 1 bash -c 'compile_lvgl "$0"'
echo "LVGL: $(ls $B/lvgl/*.o | wc -l) objects"

# 2) our files, every warning shown
MINE="carreport.c carreport_text.c carreport_pdf.c carscan_sample.c font_report.c"
rm -f $B/warnings.txt
for f in $MINE tests/test_carreport.c; do
    o=$B/obj/$(basename ${f%.c}).o
    $CC $CFLAGS -Wall -Wextra $INC -c $S/$f -o $o 2>>$B/warnings.txt ||
        { cat $B/warnings.txt; echo "COMPILE FAILED: $f"; exit 1; }
done
echo "warnings in our files (gcc -Wall -Wextra): $(grep -c 'warning:' $B/warnings.txt || true)"
cat $B/warnings.txt
OBJS=""
for f in $MINE tests/test_carreport.c; do OBJS="$OBJS $B/obj/$(basename ${f%.c}).o"; done
$CC -o $B/test_carreport $OBJS $B/lvgl/*.o -lm

# 3) the PDF checks done from outside: qpdf structure check, page size, and
#    pixel checks (Windows Python + PyMuPDF draws each page back into pixels)
cat > $B/check_pdfs.py <<'PY'
import sys, os, glob
import fitz
out = sys.argv[1]
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        fails += 1
def pix(page):
    pm = page.get_pixmap(dpi=150)
    return pm, pm.samples, pm.width, pm.height
def at(pm_data, x, y):
    pm, data, w, h = pm_data
    i = (y * w + x) * pm.n
    return tuple(data[i:i + 3])
def near(c, want, tol=10):
    return all(abs(a - b) <= tol for a, b in zip(c, want))
def count_color(pm_data, y0, y1, want, tol=10, step=2):
    pm, data, w, h = pm_data
    n = 0
    for y in range(y0, y1, step):
        for x in range(0, w, step):
            i = (y * w + x) * pm.n
            if near(tuple(data[i:i + 3]), want, tol):
                n += 1
    return n
def count_ink(pm_data, y0, y1):
    pm, data, w, h = pm_data
    n = 0
    for y in range(y0, y1, 2):
        row = data[(y * w) * pm.n:(y * w + w) * pm.n]
        if min(row) < 235:
            n += 1
    return n
def ink_x(pm_data, y0, y1):
    pm, data, w, h = pm_data
    xs = []
    for y in range(y0, y1, 2):
        for x in range(0, w, 2):
            i = (y * w + x) * pm.n
            if sum(data[i:i + 3]) < 300:
                xs.append(x)
    return (min(xs), max(xs)) if xs else (-1, -1)
GREEN_BG, RED_BG, HEAD_BG, ORANGE = (228, 243, 234), (251, 229, 227), (232, 236, 243), (200, 80, 28)

pdfs = sorted(glob.glob(os.path.join(out, '*', '*.pdf')))
check(len(pdfs) >= 12, 'all test PDFs are there (%d)' % len(pdfs))
blank_bottom = True
for p in pdfs:
    d = fitz.open(p)
    a4 = all(abs(pg.rect.width - 595.2) < 1.5 and abs(pg.rect.height - 841.9) < 1.5 for pg in d)
    if not a4:
        check(False, 'A4 pages: ' + os.path.basename(p))
    for pg in d:
        pmd = pix(pg)
        # nothing may be drawn below the bottom margin (page breaks work)
        if count_ink(pmd, 1680, pmd[3]) > 0:
            blank_bottom = False
            print('INFO something below the bottom margin: %s page %d' % (os.path.basename(p), pg.number + 1))
check(blank_bottom, 'nothing below the bottom margin on any page (page breaks work)')

def first(sub, tag):
    fs = sorted(glob.glob(os.path.join(out, sub, '*-%s.pdf' % tag)))
    return fitz.open(fs[0]) if fs else None
en, ar = first('sample', 'EN'), first('sample', 'AR')
if en and ar:
    p1 = pix(en[0])
    check(near(at(p1, 620, 420), GREEN_BG), 'healthy car: green summary box')
    check(count_color(p1, 95, 125, ORANGE, tol=40) > 40, 'orange NZEUS LAB line on top')
    lo, hi = ink_x(p1, 140, 200)
    check(lo < 100, 'English title starts on the left (x=%d)' % lo)
    a1 = pix(ar[0])
    lo, hi = ink_x(a1, 140, 200)
    check(lo > 300 and hi > 1100, 'Arabic title sits on the right (x=%d..%d)' % (lo, hi))
    check(near(at(a1, 620, 420), GREEN_BG), 'Arabic: green summary box')
else:
    check(False, 'sample PDFs found')
sick = first('sick', 'EN')
check(sick is not None and near(at(pix(sick[0]), 620, 420), RED_BG), 'unhealthy car: red summary box')
demo = first('demo', 'EN')
if demo:
    check(count_color(pix(demo[0]), 120, 330, RED_BG) > 5000, 'DEMO banner on page 1')
    check(demo.page_count > 1 and count_color(pix(demo[1]), 83, 125, RED_BG) > 2000, 'DEMO strip on page 2')
else:
    check(False, 'DEMO PDF found')
lng = first('long', 'EN')
check(lng is not None and lng.page_count >= 5 and near(at(pix(lng[2]), 600, 100), HEAD_BG),
      'long scan: table header repeated at the top of a later page')
print('%d picture checks failed' % fails)
sys.exit(fails)
PY

run_checks() {   # $1 = test program, $2 = output folder; prints PASS/FAIL lines
    rm -rf $2 && mkdir -p $2
    $1 $2 || true
    for f in $2/*/*.pdf; do
        if qpdf --check "$f" > /dev/null 2>&1; then :; else echo "FAIL qpdf --check $(basename $f)"; fi
    done
    echo "PASS qpdf --check on every PDF (if no FAIL above)"
    "$PYW" "$(wslpath -w $B/check_pdfs.py)" "$(wslpath -w $2)" || true
}

echo "--- checks ---"
run_checks $B/test_carreport $B/out | tee $B/checks.txt
NFAIL=$(grep -c '^FAIL' $B/checks.txt || true)
echo "failed checks: $NFAIL"
[ "$MODE" = quick ] && exit $NFAIL

# 4) the PDFs into reports/car, and every page as a PNG picture (150 dpi)
R="$WIN/reports/car"
mkdir -p "$R/png"
rm -f "$R"/*.pdf "$R"/png/*.png
for sub in sample long demo sick; do
    for f in $B/out/$sub/*.pdf; do
        n=$(basename $f .pdf)
        [ $sub = sample ] && dst="$R/$n.pdf" || dst="$R/$sub-$n.pdf"
        cp $f "$dst"
        pdftoppm -r 150 -png $f "$R/png/$(basename "$dst" .pdf)"
    done
done
pdfinfo "$R/Health-Scan-2026-09-29-EN.pdf" | grep -E "Pages|Page size|Producer"
ls -la "$R" | grep pdf
echo "pictures: $(ls "$R/png" | wc -l) pages in reports/car/png"

# 5) the console build of our files: MIPS cross compiler, same flags as build_nzeus_modern.sh
T=~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
MCC=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
# the console program's flags (build_nzeus_modern.sh: MIPS 74Kc, hardware FPU, one static program)
MFLAGS="-O2 -march=mips32r2 -mtune=74kc -mhard-float -EL -ffunction-sections -fdata-sections -mfp32 \
        --sysroot=$T/mipsel-buildroot-linux-gnu/sysroot"
rm -f $B/mips/warnings.txt
for f in $MINE; do
    $MCC $MFLAGS -Wall -Wextra $INC -c $S/$f -o $B/mips/${f%.c}.o 2>>$B/mips/warnings.txt ||
        { cat $B/mips/warnings.txt; echo "MIPS COMPILE FAILED: $f"; exit 1; }
done
echo "MIPS warnings in our files: $(grep -c 'warning:' $B/mips/warnings.txt || true)"
cat $B/mips/warnings.txt
$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-size $B/mips/*.o

# 6) mutants: a deliberate mistake in a copy of one file; the checks must FAIL
mutant() {   # $1 = file, $2 = description, $3 = exact text, $4 = replacement
    d=$B/mut/$(echo "$2" | tr -c 'a-zA-Z0-9' '_')
    rm -rf $d && mkdir -p $d
    cp $S/$1 $d/$1
    python3 -c 'import sys; p,a,b=sys.argv[1:4]; s=open(p).read(); n=s.count(a); open(p,"w").write(s.replace(a,b,1)); sys.exit(0 if n==1 else 1)' \
        $d/$1 "$3" "$4" || { echo "MUTANT [$2]: text not found exactly once (BROKEN MUTANT)"; return; }
    $CC $CFLAGS -w $INC -c $d/$1 -o $d/${1%.c}.o || { echo "MUTANT [$2]: does not compile"; return; }
    objs=$(echo $OBJS | sed "s#$B/obj/${1%.c}.o#$d/${1%.c}.o#")
    $CC -o $d/test_carreport $objs $B/lvgl/*.o -lm
    run_checks $d/test_carreport $d/out > $d/out.txt 2>&1
    n=$(grep -c '^FAIL' $d/out.txt || true)
    if [ "$n" -gt 0 ]; then echo "MUTANT [$2]: caught by $n failing check(s), e.g. $(grep -m1 '^FAIL' $d/out.txt)"
    else echo "MUTANT [$2]: SURVIVED (the tests missed it)"; fi
}
echo "--- mutants ---"
mutant carreport_text.c "RTL off: Arabic runs drawn in reading order" \
    'int ri = rtl ? nruns - 1 - r : r;' 'int ri = r;'
mutant carreport_text.c "no lam-alef sign" \
    'uint32_t la = (c == 0x0644 && i + 1 < n) ? lam_alef(cp[i + 1]) : 0;' 'uint32_t la = 0;'
mutant carreport.c "page break broken: tables never move to a new page" \
    'if (R.y + h <= BOTTOM) return;' 'if (1) return;'
mutant carreport.c "demo banner missing" \
    '    if (s->demo) {' '    if (0) {'
mutant carreport.c "Arabic lines not right-aligned" \
    '(align == AL_START && R.ar)' '(0)'
mutant carreport.c "summary box always green" \
    'uint32_t bg = w == 2 ? C_RED_BG' 'uint32_t bg = 0 ? C_RED_BG'
mutant carreport_pdf.c "wrong PNG predictor byte" \
    'c[-1] = 2;' 'c[-1] = 1;'
exit $NFAIL
