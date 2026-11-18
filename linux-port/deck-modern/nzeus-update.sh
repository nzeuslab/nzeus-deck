#!/bin/sh
# nzeus-update.sh check|install|restart|commit|rollback|status - "Update NZEUS
# Deck" over Wi-Fi on the SD Debian. The Update app (apps/nzeus-gui/app_update.c)
# reads these answers: check -> VERSION / NOTES / FILES <n> / BYTES <n>,
# install -> PROGRESS i n path ... DONE, a problem -> "error <reason>".
#
# The PC publishes the update: python tools/publish_update.py
# (manifest.txt = "system modern" + every file with its SHA-256, signed with
# the secret key only the PC and this console share -> manifest.sig, and the
# files as files/<sha256>). Only these kinds of file can be in it:
#   nzeus-modern             -> /usr/local/bin/nzeus-modern (the Deck itself)
#   deck-bin/NAME            -> /usr/local/lib/nzeus/deck-bin/NAME (its helpers)
#   python-examples/NAME.py  -> /usr/local/lib/nzeus/python-examples/NAME.py
#                               (the Python app's examples)
#   emoji/NAME               -> NZ_DIR/emoji/NAME (emoji pictures for text;
#                               only when the PC publishes with --emoji)
# Programs and helpers get mode 755, examples and pictures 644.
# Anything else, a list that is not signed with this console's key, or a
# file whose SHA-256 differs = nothing is installed.
#
#   check     fetch + verify the list, compare it with what is installed
#   install   download + check EVERY changed file first; then keep the old
#             copies (NZ_DIR/update/previous, and nzeus-modern.prev next to
#             the Deck), mark the update "pending" and put the new files in
#             place (temp file + rename: never half-written)
#   restart   (the app's "Restart now") restarts NZEUS Deck in the background
#             and watches it: if the new Deck does not keep running for 60 s,
#             the old files come back and the old Deck is started again;
#             if it does, the update is kept (commit)
#   commit    the update is kept;   rollback  the old files go back
#   status    one line: the installed version and whether one is pending
# If the console loses power during those 60 s the update stays "pending":
# then the Update app refuses new installs and "nzeus-update.sh rollback"
# (or "commit") over SSH settles it. /usr/local/bin/nzeus-modern.prev is
# always the Deck from before the last update.
# Settings: NZ_DIR/update/server (e.g. http://192.168.1.100:8765) and
# NZ_DIR/update/key (64 hex digits), written by publish_update.py --setup-modern.
# NZ_UPD_* change paths/programs (PC tests only). Works with dash and BusyBox ash.
HERE=$(cd "$(dirname "$0")" && pwd)
D=${NZ_DECK_DIR:-/tmp/nzeus-data/nzeus}           # NZEUS Deck's own folder (/data, kept)
U=${NZ_UPD_DIR:-$D/update}
KEY=$U/key
WORK=$U/new                                       # downloads (on /data: /tmp is 8 MB of RAM)
BACK=$U/previous                                  # the old copies of the last update
PENDING=$U/pending
BIN=${NZ_UPD_BIN:-/usr/local/bin/nzeus-modern}
DECKBIN=${NZ_UPD_DECKBIN:-/usr/local/lib/nzeus/deck-bin}
PYEX=${NZ_UPD_PYEX:-/usr/local/lib/nzeus/python-examples}
EMOJI=${NZ_UPD_EMOJI:-$D/emoji}                   # where the Deck reads them (NZ_DIR/emoji)
DECK=${NZ_UPD_DECK:-/usr/local/bin/nzeus-deck}    # start | stop | status
FETCH=${NZ_UPD_FETCH:-$HERE/nzeus-fetch}          # FETCH URL OUTFILE
PY=${NZ_UPD_PYTHON:-python3}
WATCH=${NZ_UPD_WATCH:-60}                         # seconds the new Deck must keep running
LOCK=${NZ_UPD_LOCK:-/run/nzeus-update.lock}        # volatile: cleared by a full reboot

say() { echo "$*"; }
fail() { echo "error $*"; exit 1; }

# Downloads and backups are shared, so even a check must exclude other writers.
# Refuse an abandoned lock rather than guessing whether its owner is still live.
lock_update() {
    mkdir "$LOCK" 2>/dev/null || fail "another updater is active ($LOCK); if it was interrupted, verify it has stopped before removing the lock"
    trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null' 0
    trap 'exit 130' INT
    trap 'exit 143' TERM
    echo "$$" > "$LOCK/pid" || fail "cannot record update lock owner"
}

server() {
    s=$(head -n 1 "$U/server" 2>/dev/null | tr -d '\r ')
    case "$s" in http://*|https://*) echo "${s%/}" ;; *) echo "" ;; esac
}

fetch() {   # URL OUT
    rm -f "$2"
    "$FETCH" -q --max-size 16m --timeout 20 "$1" "$2" > /dev/null 2>&1 < /dev/null && test -s "$2"
}

# a file's SHA-256 (empty if it can't be read)
sha_of() { sha256sum "$1" 2>/dev/null < /dev/null | cut -d' ' -f1; }

# the list's signature: HMAC-SHA256 with the shared key (python3's hmac, a
# constant-time compare). exit 0 = right.
sig_ok() {   # KEYFILE FILE SIGFILE
    "$PY" -c '
import hashlib, hmac, sys
key = bytes.fromhex(open(sys.argv[1]).read().strip())
if len(key) != 32:
    sys.exit(1)
mac = hmac.new(key, open(sys.argv[2], "rb").read(), hashlib.sha256).hexdigest()
sys.exit(0 if hmac.compare_digest(mac, open(sys.argv[3]).read().strip()) else 1)
' "$1" "$2" "$3" 2>/dev/null < /dev/null
}

# a plain file name? (no folders, no hidden files, only A-Z a-z 0-9 . _ -)
plain_name() {
    case "$1" in ''|.*|*/*|*[!A-Za-z0-9._-]*) return 1 ;; *) return 0 ;; esac
}

# a path from the list -> where it goes on this system ("" = not allowed)
target() {
    n=${1#*/}
    case "$1" in
        nzeus-modern) echo "$BIN" ;;
        deck-bin/*) if plain_name "$n"; then echo "$DECKBIN/$n"; else echo ""; fi ;;
        python-examples/*.py) if plain_name "$n"; then echo "$PYEX/$n"; else echo ""; fi ;;
        emoji/*) if plain_name "$n"; then echo "$EMOJI/$n"; else echo ""; fi ;;
        *) echo "" ;;
    esac
}

# the mode a path from the list gets: programs 755, examples and pictures 644
mode_of() {
    case "$1" in nzeus-modern|deck-bin/*) echo 755 ;; *) echo 644 ;; esac
}

# a MIPS32 little-endian program? (ELF, 32-bit, LSB, e_machine 8 = MIPS)
is_mips_elf() {
    h=$(od -An -tx1 -N20 "$1" 2>/dev/null | tr -d ' \n')
    case "$h" in 7f454c460101*) ;; *) return 1 ;; esac
    test "$(echo "$h" | cut -c37-40)" = 0800
}

# copy_safely FROM TO [MODE]: into TO.tmp next to TO, then rename (never
# half-written); MODE defaults to 755
copy_safely() {
    test ! -d "$2" || return 1
    mkdir -p "$(dirname "$2")" || return 1
    if cp "$1" "$2.tmp" && chmod "${3:-755}" "$2.tmp" && mv -f "$2.tmp" "$2"; then return 0; fi
    rm -f "$2.tmp"
    return 1
}

# fetch the list, check its signature, write WORK/todo (the files that differ)
get_list() {
    srv=$(server)
    test -n "$srv" || fail "no update server set (NZ_DIR/update/server: run publish_update.py --setup-modern on the PC)"
    test -s "$KEY" || fail "no update key on this console (NZ_DIR/update/key: run publish_update.py --setup-modern on the PC)"
    command -v "$PY" > /dev/null 2>&1 || fail "python3 is missing (it checks the update's signature)"
    mkdir -p "$WORK" || fail "cannot write to $U"
    fetch "$srv/manifest.txt" "$WORK/manifest.txt" || fail "cannot reach the PC ($srv) - is publish_update.py --modern running, and Wi-Fi on?"
    fetch "$srv/manifest.sig" "$WORK/manifest.sig" || fail "the update has no signature"
    sig_ok "$KEY" "$WORK/manifest.txt" "$WORK/manifest.sig" ||
        fail "the update is NOT signed with this console's key - nothing installed"
    test "$(head -n 1 "$WORK/manifest.txt")" = "NZEUS-UPDATE 1" || fail "not an NZEUS update list"
    grep -qx "system modern" "$WORK/manifest.txt" ||
        fail "this list is not for NZEUS Deck on the SD Debian (no 'system modern' line) - nothing installed"
    : > "$WORK/todo"
    files=0; bytes=0
    while read -r kind sum size path; do
        test "$kind" = file || continue
        dest=$(target "$path")
        test -n "$dest" || fail "the list has a file that is not part of NZEUS Deck: $path"
        case "$sum" in *[!0-9a-f]*) fail "bad SHA-256 in the list" ;; esac
        test ${#sum} = 64 || fail "bad SHA-256 in the list"
        case "$size" in ''|*[!0-9]*) fail "bad size in the list" ;; esac
        if test -f "$dest" && test "$(sha_of "$dest")" = "$sum"; then continue; fi
        echo "$sum $size $path" >> "$WORK/todo"
        files=$((files + 1)); bytes=$((bytes + size))
    done < "$WORK/manifest.txt"
    VERSION=$(sed -n 's/^version //p' "$WORK/manifest.txt" | head -n 1)
    NOTES=$(sed -n 's/^notes //p' "$WORK/manifest.txt" | head -n 1)
}

deck_running() { "$DECK" status 2>/dev/null | head -n 1 | grep -q "running (pid"; }

# These also run within an existing transaction; do not launch another updater.
commit_update() {
    test -f "$PENDING" || return 0
    cp "$PENDING" "$U/installed.tmp" && mv -f "$U/installed.tmp" "$U/installed" || fail "cannot record installed version (pending marker kept)"
    rm -f "$PENDING" || fail "cannot clear pending marker"
    sync
    say "ok update kept"
}

rollback_files() {
    test -f "$BACK/list" || fail "no backup of the last update (pending marker kept)"
    errors=0
    while read -r kind path; do
        dest=$(target "$path")
        test -n "$dest" || { errors=$((errors + 1)); continue; }
        if test "$kind" = old; then
            copy_safely "$BACK/files/$path" "$dest" "$(mode_of "$path")" || errors=$((errors + 1))
        elif test "$kind" = new; then
            rm -f "$dest" || errors=$((errors + 1))
        else errors=$((errors + 1)); fi
    done < "$BACK/list"
    test "$errors" = 0 || fail "rollback incomplete ($errors files); pending marker and backups kept"
    rm -f "$PENDING" || fail "cannot clear pending marker"
    sync
    say "ok the last update was undone"
}

case "$1" in check|install|restart-now|commit|rollback) lock_update ;; esac

case "$1" in
check)
    get_list
    say "VERSION ${VERSION:-?}"
    say "NOTES ${NOTES:-}"
    say "FILES $files"
    say "BYTES $bytes"
    ;;
install)
    test -f "$PENDING" && fail "the last update is not proven yet - restart NZEUS Deck first"
    get_list
    test "$files" = 0 && { say "DONE nothing to update"; exit 0; }
    # 1. download + check EVERY file first (nothing is changed yet)
    mkdir -p "$WORK/files"
    i=0
    while read -r sum size path; do
        i=$((i + 1))
        say "PROGRESS $i $files $path"
        f="$WORK/files/$sum"
        if ! { test -f "$f" && test "$(sha_of "$f")" = "$sum"; }; then
            fetch "$(server)/files/$sum" "$f" || fail "download failed: $path - nothing installed"
            test "$(sha_of "$f")" = "$sum" || { rm -f "$f"; fail "damaged download: $path - nothing installed"; }
        fi
        if test "$path" = nzeus-modern; then
            is_mips_elf "$f" || fail "the new nzeus-modern is not a MIPS program - nothing installed"
        fi
    done < "$WORK/todo"
    # 2. keep the old copies (the backup of the update before is replaced)
    rm -rf "$BACK" && mkdir -p "$BACK" || fail "cannot make the backup folder"
    : > "$BACK/list"
    while read -r sum size path; do
        dest=$(target "$path")
        if test -f "$dest"; then
            copy_safely "$dest" "$BACK/files/$path" || fail "backup failed: $path - nothing installed"
            echo "old $path" >> "$BACK/list"
        else
            echo "new $path" >> "$BACK/list"          # (did not exist: a rollback removes it)
        fi
    done < "$WORK/todo"
    if grep -q " nzeus-modern$" "$WORK/todo" && test -f "$BIN"; then
        copy_safely "$BIN" "$BIN.prev" || fail "could not keep the old Deck as $BIN.prev - nothing installed"
    fi
    sync
    # 3. pending FIRST, then the files
    echo "${VERSION:-?}" > "$PENDING" || fail "cannot mark update pending - nothing installed"
    sync
    while read -r sum size path; do
        copy_safely "$WORK/files/$sum" "$(target "$path")" "$(mode_of "$path")" ||
            { say "error installing $path: undoing"; rollback_files; exit 1; }
    done < "$WORK/todo"
    sync
    rm -rf "$WORK/files"
    say "DONE ${VERSION:-?} installed - restart NZEUS Deck to start it"
    ;;
restart)
    # in the background, in a session of its own: it stops the Deck that started it
    mkdir -p "$U"
    setsid sh "$0" restart-now >> "$U/restart.log" 2>&1 < /dev/null &
    say "ok NZEUS Deck restarts now"
    ;;
restart-now)
    echo "$(date '+%Y-%m-%d %H:%M:%S') restarting NZEUS Deck"
    sleep 1                                          # (the app's screen finishes)
    "$DECK" stop
    if "$DECK" start; then
        n=0
        while test "$n" -lt "$WATCH" && deck_running; do sleep 2; n=$((n + 2)); done
        if deck_running; then
            commit_update
            exit 0
        fi
    fi
    if test -f "$PENDING"; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') the new NZEUS Deck stopped: putting the old one back"
        rollback_files
        "$DECK" start
    fi
    ;;
commit)
    commit_update
    ;;
rollback)
    rollback_files
    ;;
status)
    v=$(cat "$U/installed" 2>/dev/null)
    if test -f "$PENDING"; then say "pending $(cat "$PENDING") (installed: ${v:-unknown})"; else say "installed ${v:-unknown}"; fi
    ;;
*)
    echo "usage: nzeus-update.sh check|install|restart|commit|rollback|status"; exit 2 ;;
esac
