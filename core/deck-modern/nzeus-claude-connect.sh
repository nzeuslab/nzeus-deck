#!/bin/sh
# nzeus-claude-connect.sh BRAIN_HOST BRAIN_PORT USER
# nzeus-claude-connect.sh --make-key
#
# NZEUS Deck's Claude app, "Connect", on the SD Debian: opens Claude Code on
# the brain (home PC or phone) and shows it in the app's terminal.
# The stock version (brain/console) had to reach the brain through the
# user-space network (nzeus-netd) and carried a reverse tunnel for "Claude
# access". This kernel has TCP/IP, so it is simply:
#     dbclient -t -i KEY -p PORT USER@HOST claude
# The brain's authorized_keys line for this console's key forces Claude Code
# in its nzeus-claude folder anyway (brain/pc/setup_pc_ssh.ps1, android
# setup.sh); "claude" is only asked for in case the brain does not force it.
# No tunnel back: "Claude access" is not on this system yet (see
# core/deck-modern/CLAUDE-SSH.md).
#
# The key: $NZ_CONSOLE_KEY (the Claude app unlocks it with the PIN into RAM,
# /tmp/nzeus-run/console_key, for this one connection) - then nothing is
# made or written. Without it: NZ_DIR/claude/console_key (manual setup;
# --make-key makes it and prints where its public half is).
# The brain's host key is accepted the FIRST time (-y) and kept in
# NZ_DIR/claude/.ssh/known_hosts; a changed key later is refused.
. "$(dirname "$0")/nzeus-common.sh"

CARD_KEY="$NZ_CLAUDE/console_key"
PUB="$NZ_CLAUDE/console_key.pub"

make_key() {
    mkdir -p "$NZ_CLAUDE" || return 1
    if [ ! -s "$CARD_KEY" ]; then
        echo "Making this console's key (first time only)..."
        "$NZ_DBMULTI" dropbearkey -t ed25519 -f "$CARD_KEY" > /dev/null 2>&1
        [ -s "$CARD_KEY" ] || { echo "error: could not make $CARD_KEY" >&2; return 1; }
        rm -f "$PUB"
    fi
    if [ ! -s "$PUB" ]; then
        # dropbearkey -y prints the public key; keep "type key" and add a name
        "$NZ_DBMULTI" dropbearkey -y -f "$CARD_KEY" 2>/dev/null \
            | awk '/^(ssh-|ecdsa-)/ { print $1 " " $2 " nzeus-console" }' > "$PUB"
        [ -s "$PUB" ] || { echo "error: could not write $PUB" >&2; return 1; }
    fi
    return 0
}

if [ "$1" = "--make-key" ]; then
    make_key || exit 1
    echo "Console public key: $PUB"
    echo "Copy it to the brain (see core/deck-modern/CLAUDE-SSH.md)."
    exit 0
fi
if [ $# -lt 3 ]; then
    echo "usage: $0 BRAIN_HOST BRAIN_PORT USER   (or --make-key)" >&2
    exit 2
fi
BRAIN_HOST=$1
BRAIN_PORT=$2
BRAIN_USER=$3
# plain characters only (they go on dbclient's command line)
case "$BRAIN_HOST" in ''|-*|*[!A-Za-z0-9.:-]*) echo "error: bad brain address: $BRAIN_HOST" >&2; exit 2 ;; esac
case "$BRAIN_PORT" in ''|*[!0-9]*) echo "error: bad brain port: $BRAIN_PORT" >&2; exit 2 ;; esac
case "$BRAIN_USER" in ''|-*|*[!A-Za-z0-9._-]*) echo "error: bad user name: $BRAIN_USER" >&2; exit 2 ;; esac

# The PC at home or away: away from the home Wi-Fi its home address can't be
# reached, so the home's public address (settings.ini claude_pc_public, through
# the router's port forward) is tried first there - nz_pick_pc decides.
PC_HOME=$(nz_setting claude_pc_host)
AWAY="${NZ_BRAIN_AWAY-$(nz_setting claude_pc_public)}"
case "$AWAY" in ''|-*|*[!A-Za-z0-9.:-]*) AWAY="" ;; esac
if [ "$BRAIN_HOST" = "${PC_HOME:-}" ]; then    # (only the PC; the phone brain stays as it is)
    set -- $(nz_pick_pc "$BRAIN_HOST" "$AWAY")
    BRAIN_HOST=$1
fi

if [ -n "$NZ_CONSOLE_KEY" ]; then
    KEY=$NZ_CONSOLE_KEY
    [ -s "$KEY" ] || { echo "error: the console key $KEY is missing or empty" >&2; exit 1; }
else
    make_key || exit 1
    KEY=$CARD_KEY
fi

# HOME = the claude folder, so known_hosts lives in NZ_DIR/claude/.ssh
HOME="$NZ_CLAUDE"
TERM="${TERM:-xterm-256color}"
[ "$TERM" = dumb ] && TERM=xterm-256color
export HOME TERM
mkdir -p "$NZ_CLAUDE"

printf 'Connecting to %s@%s port %s ...\r\n' "$BRAIN_USER" "$BRAIN_HOST" "$BRAIN_PORT"   # (\r: raw terminal)
#  -t     a terminal on the brain (Claude Code's full screen)
#  -y     accept the brain's host key the FIRST time (a changed key is refused)
#  -K 30  keepalive every 30 s, so a dead Wi-Fi link is noticed
"$NZ_DBMULTI" dbclient -t -y -K 30 -i "$KEY" -p "$BRAIN_PORT" "$BRAIN_USER@$BRAIN_HOST" claude
rc=$?
if [ $rc -ne 0 ]; then
    echo ""
    echo "Connection ended with code $rc. Common reasons:"
    echo " - brain off / wrong address / Wi-Fi down"
    echo " - this console's key is not on the brain yet ($PUB)"
    echo " - the brain's host key changed (delete $NZ_CLAUDE/.ssh/known_hosts if you know why)"
fi
exit $rc
