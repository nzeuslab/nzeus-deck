# nzeus-common.sh - shared settings for NZEUS Deck's brain scripts on the SD
# Debian (nzeus-claude-connect.sh, nzeus-wa.sh).
# Loaded with  . nzeus-common.sh  - not run by itself.
# The kernel has TCP/IP, so dbclient simply connects to the brain itself.
# Every path can be changed with an environment variable (PC tests do).

# this folder (= NZ_DIR/bin -> /usr/local/lib/nzeus/deck-bin)
NZ_BIN="${NZ_BIN:-$(cd "$(dirname "$0")" && pwd)}"
# NZEUS Deck's own folder (settings.ini, claude/, whatsapp/): the Deck's NZ_DIR
NZ_ROOT="${NZ_ROOT:-${NZ_DECK_DIR:-/tmp/nzeus-data/nzeus}}"
# Claude files: keys, known_hosts
NZ_CLAUDE="${NZ_CLAUDE:-$NZ_ROOT/claude}"
# Dropbear's programs (see dropbearmulti in this folder)
if [ -z "$NZ_DBMULTI" ]; then
    if [ -f "$NZ_BIN/dropbearmulti" ]; then NZ_DBMULTI="$NZ_BIN/dropbearmulti"; else NZ_DBMULTI=dropbearmulti; fi
fi

# nz_setting KEY: its value in the Deck's settings.ini ("" when it isn't there)
nz_setting() {
    sed -n "s/^$1=//p" "${NZ_SETTINGS:-$NZ_ROOT/settings.ini}" 2>/dev/null | head -n 1 | tr -d '\r'
}

# nz_pick_pc HOME_ADDR AWAY_ADDR: the PC's addresses in the order to try them.
# At home only the home address works, away only the home's public address
# (settings.ini claude_pc_public, through the router's port forward): when our
# gateway is on the home address's network (same first 3 numbers) -> home
# first, else public first. NZ_GW overrides the gateway (tests).
nz_pick_pc() {
    if [ -z "$2" ] || [ "$2" = "$1" ]; then echo "$1"; return; fi
    gw="${NZ_GW-$(ip -4 route show default 2>/dev/null | awk '$1 == "default" { print $3; exit }')}"
    if [ -z "$gw" ]; then echo "$1 $2"; return; fi
    case "$1" in
        "${gw%.*}".*) echo "$1 $2" ;;
        *) echo "$2 $1" ;;
    esac
}

# only plain path characters (letters, digits, / . _ -)
safe_path() {
    case "$1" in
        ''|*[!A-Za-z0-9/._-]*) return 1 ;;
        *) return 0 ;;
    esac
}
