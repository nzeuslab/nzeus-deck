#!/bin/sh
# nzeus-wa.sh COMMAND...  -  asks the WhatsApp bridge on the PC (the "brain").
#
#   nzeus-wa.sh status | chats | read CHAT_ID [N] | send CHAT_ID BASE64_TEXT
#   nzeus-wa.sh photo CHAT_ID MSG_ID OUTFILE       one photo of a chat -> OUTFILE (a JPEG)
#   nzeus-wa.sh voice CHAT_ID MSG_ID OUTFILE       one voice message -> OUTFILE (a WAV)
#   nzeus-wa.sh sendphoto CHAT_ID FILE [BASE64_CAPTION]   send a picture (JPEG/PNG, max 4 MB)
#   nzeus-wa.sh page open BASE64_URL OUTFILE       PC view: the PC's Chrome opens a web page;
#   nzeus-wa.sh page shot [DY] OUTFILE               its picture (a 640 x 480 JPEG) -> OUTFILE,
#   nzeus-wa.sh page click X Y OUTFILE               its address <TAB> title -> OUTFILE.info
#   nzeus-wa.sh page type BASE64_TEXT OUTFILE        (shot: scroll DY pixels first; type: the
#   nzeus-wa.sh page key enter|back OUTFILE           text, then Enter)
#   nzeus-wa.sh page close                         the PC's page tab closes
#   nzeus-wa.sh --make-key      make this console's WhatsApp key (first time)
#
# The console logs in to the PC with its WhatsApp key ($NZ_WA_KEY: the NZEUS app
# decrypts it with the PIN into RAM). On the PC that key may ONLY run wa-gate.js
# (brain/whatsapp), which checks the command again and asks the bridge.
# Unlike Claude: no terminal, no tunnel back to the console - one question,
# one answer (printed exactly as wa-gate.js wrote it).
# The PC: NZ_WA_HOST / NZ_WA_PORT / NZ_WA_USER (the NZEUS app passes the Claude
# brain's address). Busybox ash compatible.
# THE SD DEBIAN VERSION (linux-port/deck-modern) of brain/console/nzeus-wa.sh:
# same arguments, answers and exit codes; only the "no TCP/IP" way (dbclient
# through nzeus-nc / nzeus-netd) is gone - this kernel has TCP/IP, so dbclient
# (Debian's, via dropbearmulti in this folder) connects to the PC directly.
# The PC side is unchanged (brain/whatsapp wa-gate.js; CLAUDE-SSH.md).

. "$(dirname "$0")/nzeus-common.sh"

NZ_WA="${NZ_WA:-$NZ_ROOT/whatsapp}"
CARD_KEY="$NZ_WA/wa_key"                 # only while making it; the app keeps it locked (wa_key.enc)
PUB="$NZ_WA/wa_key.pub"

if [ "$1" = "--make-key" ]; then
    mkdir -p "$NZ_WA" || exit 1
    rm -f "$CARD_KEY" "$PUB"
    "$NZ_DBMULTI" dropbearkey -t ed25519 -f "$CARD_KEY" > /dev/null 2>&1
    [ -s "$CARD_KEY" ] || { echo "error: could not make the WhatsApp key"; exit 1; }
    "$NZ_DBMULTI" dropbearkey -y -f "$CARD_KEY" 2>/dev/null \
        | awk '/^(ssh-|ecdsa-)/ { print $1 " " $2 " nzeus-console-wa" }' > "$PUB"
    [ -s "$PUB" ] || { echo "error: could not write $PUB"; exit 1; }
    echo "ok $CARD_KEY"
    exit 0
fi

KEY="${NZ_WA_KEY:-}"
HOST="${NZ_WA_HOST:-}"
PORT="${NZ_WA_PORT:-22}"
USER_="${NZ_WA_USER:-}"
[ -n "$HOST" ] && [ -n "$USER_" ] || { echo "error: no PC set yet (Claude app > Address)"; exit 2; }
[ -n "$KEY" ] && [ -s "$KEY" ] || { echo "error: the WhatsApp key is locked (open the app and type the PIN)"; exit 1; }
[ $# -ge 1 ] || { echo "error: usage: nzeus-wa.sh status|chats|read ID [N]|send ID BASE64|photo ID MSG FILE|voice ID MSG FILE|sendphoto ID FILE|page ..."; exit 2; }

# photo / voice / sendphoto: the file names stay on the console (only the IDs go to the PC)
OUT=""
FILE=""
WHAT=""                                  # photo / voice: what is downloaded (for the messages)
PAGE=""                                  # 1 = a PC view page picture (an address/title line comes first)
case "$1" in
page)
    # PC view: the last word is OUTFILE (except "page close")
    case "$2 $#" in
    "open 4"|"type 4"|"key 4"|"shot 3"|"shot 4"|"click 5") ;;
    "close 2") ;;
    *) echo "error: usage: nzeus-wa.sh page open B64|shot [DY]|click X Y|type B64|key enter/back OUTFILE, or page close"; exit 2 ;;
    esac
    if [ "$2" != close ]; then
        eval "OUT=\${$#}"                       # (the last word)
        case "$OUT" in /*) ;; *) echo "error: OUTFILE must be a full path"; exit 2 ;; esac
        case "$OUT" in *[!A-Za-z0-9/._-]*) echo "error: bad OUTFILE"; exit 2 ;; esac
        WHAT="page picture"
        PAGE=1
        case $# in
        3) set -- "$1" "$2" ;;
        4) set -- "$1" "$2" "$3" ;;
        5) set -- "$1" "$2" "$3" "$4" ;;
        esac
    fi
    ;;
photo|voice)
    [ $# -eq 4 ] || { echo "error: usage: nzeus-wa.sh $1 CHAT_ID MSG_ID OUTFILE"; exit 2; }
    OUT="$4"
    case "$OUT" in /*) ;; *) echo "error: OUTFILE must be a full path"; exit 2 ;; esac
    case "$3" in ''|*[!0-9A-Za-z@._-]*) echo "error: bad message ID"; exit 2 ;; esac
    [ ${#3} -ge 5 ] && [ ${#3} -le 160 ] || { echo "error: bad message ID"; exit 2; }
    WHAT=photo; [ "$1" = voice ] && WHAT="voice message"
    set -- "$1" "$2" "$3"
    ;;
sendphoto)
    [ $# -eq 3 ] || [ $# -eq 4 ] || { echo "error: usage: nzeus-wa.sh sendphoto CHAT_ID FILE [BASE64_CAPTION]"; exit 2; }
    FILE="$3"
    [ -f "$FILE" ] || { echo "error: no such picture: $FILE"; exit 1; }
    size=$(wc -c < "$FILE")
    [ "$size" -gt 0 ] && [ "$size" -le 4194304 ] || { echo "error: the picture is too big (4 MB at most)"; exit 1; }
    if [ $# -eq 4 ]; then set -- sendphoto "$2" "$4"; else set -- sendphoto "$2"; fi
    ;;
esac

# everything that goes into the command line: plain characters only
case "$HOST" in ''|-*|*[!A-Za-z0-9.:-]*) echo "error: bad PC address"; exit 2 ;; esac
case "$PORT" in ''|*[!0-9]*) echo "error: bad PC port"; exit 2 ;; esac
case "$USER_" in ''|-*|*[!A-Za-z0-9._-]*) echo "error: bad user name"; exit 2 ;; esac
for w in "$@"; do
    case "$w" in *[!A-Za-z0-9@._+/=-]*) echo "error: bad request"; exit 2 ;; esac
done

# known_hosts in nzeus/whatsapp/.ssh (the PC's host key is accepted the first
# time, a changed key later is refused)
HOME="$NZ_WA"
export HOME
mkdir -p "$NZ_WA"

# Which address of the PC: at home its home address, away from home the
# home's public address (settings.ini claude_pc_public, through the router's
# port forward) - nz_pick_pc in nzeus-common.sh puts the right one first.
AWAY="${NZ_WA_HOST_AWAY-$(nz_setting claude_pc_public)}"
case "$AWAY" in ''|-*|*[!A-Za-z0-9.:-]*) AWAY="" ;; esac
HOSTS=$(nz_pick_pc "$HOST" "$AWAY")
# Only questions are asked again at the other address when the first one
# can't be reached; a message or picture is never sent twice.
case "$1" in send|sendphoto) HOSTS=${HOSTS%% *} ;; esac
# (PC view: a click or typing is never done twice either)
case "$1 $2" in "page click"|"page type"|"page key") HOSTS=${HOSTS%% *} ;; esac
REQ="$*"

# Keep SSH diagnostics private and temporary. Only fixed explanations reach the
# app; raw host banners, paths and account details are never copied to its UI.
SSH_ERROR=$(umask 077; mktemp "${TMPDIR:-/tmp}/nzeus-wa-ssh.XXXXXX" 2>/dev/null) || {
    echo "error: Could not prepare the PC connection. Check free memory."; exit 1;
}
trap 'rm -f "$SSH_ERROR"' 0
trap 'exit 1' 1 2 15

ssh_failure() {
    diagnostic=$(head -c 4096 "$SSH_ERROR")
    case "$diagnostic" in
    *"host key mismatch"*|*"HOST IDENTIFICATION HAS CHANGED"*|*"Didn't validate host key"*)
        echo "error: PC identity changed. Verify its host key before reconnecting." ;;
    *"Failed loading keyfile"*|*"Error reading key"*)
        echo "error: Could not load the WhatsApp key. Reopen WhatsApp and enter your PIN." ;;
    *"No auth methods could be used"*|*"Permission denied (publickey"*|*"No more authentication methods"*)
        echo "error: PC rejected the WhatsApp key. Register this console's wa_key.pub on the PC." ;;
    *"Connection refused"*|*"Connection timed out"*|*"No route to host"*|*"Network is unreachable"*|*"Error resolving"*|*"Connect failed"*)
        echo "error: Could not reach the PC. Check its address, port and internet connection." ;;
    *) echo "error: Secure PC connection failed. Check the PC address, port and access setup." ;;
    esac
}

# ask HOST: one try at one address, the answer goes to stdout.
#   -y  accept the PC's host key the first time   -T  no terminal (so any bytes
#   pass through unchanged, a picture too)   -K 30  keepalive
ask() {
    set -- "$USER_@$1" "$REQ"
    if [ -n "$FILE" ]; then
        "$NZ_DBMULTI" dbclient -y -T -K 30 -i "$KEY" -p "$PORT" "$@" < "$FILE" 2>>"$SSH_ERROR"   # (the picture's bytes on stdin)
    else
        "$NZ_DBMULTI" dbclient -y -T -K 30 -i "$KEY" -p "$PORT" "$@" < /dev/null 2>>"$SSH_ERROR"
    fi
}

# wav_check FILE: 0 = a whole WAV, 1 = not a WAV, 2 = cut off.
# A WAV starts with "RIFF", its size (4 bytes, lowest first), "WAVE"; a cut-off
# answer can decode fine but is shorter than that size says.
wav_check() {
    h=$(dd if="$1" bs=12 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
    case "$h" in 52494646????????57415645) ;; *) return 1 ;; esac
    s=$(printf '%s' "$h" | cut -c9-16)                  # the size, lowest byte first: turn it round
    s=$(printf '%s' "$s" | cut -c7-8)$(printf '%s' "$s" | cut -c5-6)$(printf '%s' "$s" | cut -c3-4)$(printf '%s' "$s" | cut -c1-2)
    case "$s" in ffffffff|00000000) return 0 ;; esac   # (no size filled in: can't tell, accept it)
    [ "$(wc -c < "$1")" -ge $((0x$s + 8)) ] || return 2
    return 0
}

if [ -n "$OUT" ]; then
    # photo / voice: the answer is "ok", then the JPEG / WAV in base64 (76 letters
    # a line). It is decoded into OUTFILE.tmp (on the card, not in RAM); only a
    # whole file is moved to OUTFILE. A page: "ok", its address <TAB> title
    # (-> OUTFILE.info), then the JPEG.
    for h in $HOSTS; do
        rm -f "$OUT.tmp" "$OUT.info.tmp"
        ask "$h" | {
            IFS= read -r first
            first=$(printf '%s' "$first" | tr -d '\r')
            [ -n "$first" ] || exit 5                   # (nothing came: the PC wasn't reached)
            if [ "$first" != ok ]; then
                cat > /dev/null                         # (let the PC finish)
                case "$first" in "error: "*) echo "$first" ;; *) echo "error: no $WHAT came from the PC" ;; esac
                exit 3
            fi
            if [ -n "$PAGE" ]; then IFS= read -r info; printf '%s\n' "$info" | tr -d '\r' > "$OUT.info.tmp"; fi
            base64 -d > "$OUT.tmp" 2>/dev/null || exit 4
        }
        rc=$?
        [ $rc -eq 5 ] || break                          # (reached: don't try the other address)
    done
    case $rc in
    0) ;;
    3) rm -f "$OUT.tmp" "$OUT.info.tmp"; exit 1 ;;      # (the PC's error was printed)
    5) rm -f "$OUT.tmp" "$OUT.info.tmp"; ssh_failure; exit 1 ;;
    *) rm -f "$OUT.tmp" "$OUT.info.tmp"; echo "error: the $WHAT could not be decoded"; exit 1 ;;
    esac
    [ -s "$OUT.tmp" ] || { rm -f "$OUT.tmp"; echo "error: the $WHAT was empty"; exit 1; }
    if [ "$1" = voice ]; then
        wav_check "$OUT.tmp"
        case $? in
        0) ;;
        1) rm -f "$OUT.tmp"; echo "error: the voice message is not a WAV file"; exit 1 ;;
        *) rm -f "$OUT.tmp"; echo "error: the voice message came only partly (try again)"; exit 1 ;;
        esac
    else
        # a whole JPEG ends with the bytes FF D9: a cut-off answer can decode fine but lacks them
        case "$(tail -c 2 "$OUT.tmp" | od -An -tx1 | tr -d ' \n')" in
        ffd9) ;;
        *) rm -f "$OUT.tmp" "$OUT.info.tmp"; echo "error: the $WHAT came only partly (try again)"; exit 1 ;;
        esac
    fi
    mv -f "$OUT.tmp" "$OUT" || { rm -f "$OUT.tmp"; echo "error: could not save the $WHAT"; exit 1; }
    [ -z "$PAGE" ] || mv -f "$OUT.info.tmp" "$OUT.info" 2>/dev/null
    echo ok
    exit 0
fi

# everything else: the answer is printed as the PC wrote it
for h in $HOSTS; do
    ans=$(ask "$h")
    rc=$?
    [ -n "$ans" ] && { printf '%s\n' "$ans"; exit $rc; }
done
ssh_failure
exit 255                                                # no address returned an answer
