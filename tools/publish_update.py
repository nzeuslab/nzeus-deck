"""publish_update.py - offers an NZEUS Deck update to the console over Wi-Fi.

NZEUS Deck on the SD Debian (nzeus-modern; the console side is
core/deck-modern/nzeus-update.sh, Tools > Update in the Deck):

    python tools/publish_update.py --notes "what is new"     build + serve (Ctrl+C stops)
    python tools/publish_update.py --setup-modern root@CONSOLE-IP [--ssh-key FILE]
                                                             ONCE: copies the secret key +
                                                             this PC's address over SSH
    python tools/publish_update.py --build-only [--out DIR]  just build (tests)
    python tools/publish_update.py --emoji ...               also ship the emoji pictures
                                                             (tools/make_emoji.py first)
--modern is still accepted (it was needed while a second, older system existed;
now it changes nothing).

What it makes (in update_out/): manifest.txt = a "system modern" line and every
file with its SHA-256 and size, signed with the secret key (manifest.sig,
HMAC-SHA256), and the files themselves as files/<sha256>. The console installs
nothing that isn't signed with the same key, and checks every file. The key
lives in %USERPROFILE%\\.ssh\\nzeus_update.key and on the console
(NZ_DIR/update/key). The list holds only these kinds of file (the console
refuses anything else):
    nzeus-modern             apps/frontend-modern/mips/nzeus-modern (build_nzeus_modern.sh)
    deck-bin/NAME            core/deck-modern/NAME (required-files.txt) and
                             deck-bin/nzeus-bt.bin = core/deck-modern/bt/nzeus-bt-mipsel
    python-examples/NAME.py  core/deck-modern/python-examples/*.py
    emoji/NAME               core/deck-modern/build/emoji/ (only with --emoji)
The same files the system image gets (ports/r36s-hichip/rootfs/debian/apply-overlay.sh).
"""
import argparse, datetime, functools, hashlib, hmac, http.server, os, secrets, shutil, socket, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "update_out")
KEYFILE = os.environ.get("NZ_UPD_KEYFILE") or os.path.join(os.environ.get("USERPROFILE", os.path.expanduser("~")), ".ssh", "nzeus_update.key")
PORT = 8765
# what NZEUS Deck on the SD Debian is made of (same places as
# ports/r36s-hichip/rootfs/debian/apply-overlay.sh takes them from)
MODERN_BIN = os.environ.get("NZ_UPD_MODERN_BIN") or os.path.join(HERE, "..", "apps", "frontend-modern", "mips", "nzeus-modern")
DECK_MODERN = os.environ.get("NZ_UPD_DECK_MODERN") or os.path.join(HERE, "..", "core", "deck-modern")
EMOJI_DIR = os.environ.get("NZ_UPD_EMOJI_DIR")       # default: DECK_MODERN/build/emoji (make_emoji.py)
MODERN_DIR = "/data/nzeus/deck/nzeus/update"         # the console's NZ_DIR/update (kept on /data)
NAME_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")


def plain_name(name):
    """A file name the console accepts: no folders, not hidden, A-Z a-z 0-9 . _ -"""
    return bool(name) and not name.startswith(".") and all(c in NAME_CHARS for c in name)


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def load_key(make=False):
    if not os.path.exists(KEYFILE):
        if not make:
            sys.exit(f"no update key yet ({KEYFILE}): run with --setup-modern first")
        os.makedirs(os.path.dirname(KEYFILE), exist_ok=True)
        with open(KEYFILE, "w") as f:
            f.write(secrets.token_hex(32) + "\n")          # 32 random bytes as 64 hex digits
        print(f"made a new update key: {KEYFILE}")
    with open(KEYFILE) as f:
        key_hex = f.read().strip()
    return key_hex, bytes.fromhex(key_hex)


def deck_files(emoji=False):
    """[(path in the list, file on this PC)] - exits on anything missing or wrong."""
    if not os.path.isfile(MODERN_BIN):
        sys.exit(f"{MODERN_BIN} is missing: run  bash build_nzeus_modern.sh  first")
    with open(MODERN_BIN, "rb") as f:
        head = f.read(20)
    if head[:6] != b"\x7fELF\x01\x01" or head[18:20] != b"\x08\x00":
        sys.exit(f"{MODERN_BIN} is not a MIPS32 little-endian program")
    files = [("nzeus-modern", MODERN_BIN)]
    # the Deck's helpers: an explicit list, every one must be there
    inventory = os.path.join(DECK_MODERN, "required-files.txt")
    with open(inventory) as f:
        names = f.read().splitlines()
    if not names or len(names) != len(set(names)):
        sys.exit("missing or duplicate Deck helper inventory")
    for name in names:
        if not plain_name(name):
            sys.exit(f"invalid Deck helper name: {name}")
        full = os.path.join(DECK_MODERN, name)
        if not os.path.isfile(full):
            sys.exit(f"required Deck helper missing: {name}")
        files.append(("deck-bin/" + name, full))
    # the Bluetooth program (static MIPS), checked against its SHA256SUMS
    bt = os.path.join(DECK_MODERN, "bt", "nzeus-bt-mipsel")
    want = ""
    with open(os.path.join(DECK_MODERN, "bt", "SHA256SUMS")) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[1] == "nzeus-bt-mipsel":
                want = parts[0]
    if not os.path.isfile(bt) or not want or sha256_file(bt) != want:
        sys.exit("core/deck-modern/bt/nzeus-bt-mipsel is missing or does not match its SHA256SUMS")
    files.append(("deck-bin/nzeus-bt.bin", bt))
    # the Python app's examples
    pyex = os.path.join(DECK_MODERN, "python-examples")
    examples = sorted(n for n in os.listdir(pyex) if n.endswith(".py"))
    if not examples:
        sys.exit(f"no Python examples in {pyex}")
    for name in examples:
        if not plain_name(name):
            sys.exit(f"invalid Python example name: {name}")
        files.append(("python-examples/" + name, os.path.join(pyex, name)))
    # the emoji pictures (about 1400 small files): only when asked for
    if emoji:
        em = EMOJI_DIR or os.path.join(DECK_MODERN, "build", "emoji")
        if not (os.path.isfile(os.path.join(em, "index.txt")) and os.path.isfile(os.path.join(em, "LICENSE.txt"))):
            sys.exit(f"no emoji pack in {em}: run  python tools/make_emoji.py  first")
        for name in sorted(os.listdir(em)):
            if (name.endswith(".bin") or name in ("index.txt", "LICENSE.txt")) and plain_name(name):
                files.append(("emoji/" + name, os.path.join(em, name)))
    return files


def build_modern(out, notes, key, emoji=False):
    """manifest (system modern) + files/<sha256> for NZEUS Deck on the SD Debian."""
    files = deck_files(emoji)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "files"))
    lines = ["NZEUS-UPDATE 1",
             "system modern",
             "version " + datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
             "notes " + " ".join(notes.split())]               # (one line)
    for rel, full in files:
        with open(full, "rb") as f:
            data = f.read()
        sha = hashlib.sha256(data).hexdigest()
        lines.append(f"file {sha} {len(data)} {rel}")
        dest = os.path.join(out, "files", sha)
        if not os.path.exists(dest):
            with open(dest, "wb") as f:
                f.write(data)
    manifest = ("\n".join(lines) + "\n").encode()
    with open(os.path.join(out, "manifest.txt"), "wb") as f:
        f.write(manifest)
    with open(os.path.join(out, "manifest.sig"), "w") as f:
        f.write(hmac.new(key, manifest, hashlib.sha256).hexdigest() + "\n")
    print(f"NZEUS Deck (SD Debian) update built: {len(files)} files listed, signed ({out})")


def setup_modern(target, ssh_key, key_hex):
    """--setup-modern: the key + this PC's address onto the SD Debian, over SSH.
    The key goes through ssh's input (never on a command line), into a temp
    file next to it, then a rename; mode 600."""
    url = f"http://{my_ip()}:{PORT}"
    cmd = ["ssh", "-o", "ConnectTimeout=10"]
    if ssh_key:
        cmd += ["-i", ssh_key]
    d = MODERN_DIR
    remote = (f"umask 077 && mkdir -p {d} && cat > {d}/key.tmp && mv -f {d}/key.tmp {d}/key && "
              f"echo {url} > {d}/server && sync && echo ok")
    r = subprocess.run(cmd + [target, remote], input=key_hex + "\n", capture_output=True, text=True)
    if r.returncode != 0 or "ok" not in r.stdout:
        sys.exit(f"ssh {target} failed: {r.stderr.strip() or r.stdout.strip()} (is the console on Wi-Fi / the cable?)")
    print(f"console set up: it will fetch updates from {url}")


def my_ip():
    """This PC's address on the home network (the console must reach it)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 9))                      # (nothing is sent: picks the LAN card)
        return s.getsockname()[0]
    finally:
        s.close()


def serve(out):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=out)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), handler)
    print(f"serving the update at http://{my_ip()}:{PORT}  - on the console: Tools > Update.  Ctrl+C stops.")
    print("(Windows may ask whether Python may use the network: allow PRIVATE networks only.)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("stopped")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--notes", default="NZEUS update")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--modern", action="store_true", help="accepted for old commands; changes nothing")
    ap.add_argument("--emoji", action="store_true", help="also ship the emoji pictures (tools/make_emoji.py)")
    ap.add_argument("--setup-modern", metavar="USER@HOST", help="once: key + address to the SD Debian over SSH")
    ap.add_argument("--ssh-key", help="private key file for --setup-modern")
    a = ap.parse_args()
    key_hex, key = load_key(make=bool(a.setup_modern))
    if a.setup_modern:
        setup_modern(a.setup_modern, a.ssh_key, key_hex)
        return
    build_modern(a.out, a.notes, key, a.emoji)
    if not a.build_only:
        serve(a.out)


if __name__ == "__main__":
    main()
