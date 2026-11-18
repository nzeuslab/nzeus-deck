#!/usr/bin/env python3
"""sync-managed-plan.py - DRY-RUN ONLY plan for a managed-file SYNC of the SD Debian.

Why: a full reinstall of the nzroot image would also reset the card's Linux
area, and my /data must never be erased. A SYNC copies only the
NZEUS-managed files that differ, nothing else, no packages.

This program NEVER touches the console or a card. It has no network code and
runs no commands. It only reads JSON files and prints/writes a plan. Doing
the plan is a separate step I approve, with the console connected, using
the project's verified update path (backup, copy, readback, restart watch).

  sync-managed-plan.py collect PROVENANCE.json
      print the READ-ONLY shell command to run on the console (by a person or
      the approved session tool) to list hashes of the managed paths. Its
      output, saved as a text file, is the LIVE file for "plan".
  sync-managed-plan.py plan PROVENANCE.json LIVE.txt [--out PLAN.md]
      compare the image's managed files with the live hashes; write the plan.
  sync-managed-plan.py docs PROVENANCE.json LIVE-KNOWN.json [--out REPORT.md]
      the same comparison against what the repo docs say is on the live card
      (manifest/live-card-known-*.json); used when the console is offline.

Exit 0 always for a successfully written plan; the plan itself says what differs.
"""
import json
import sys

# Paths a sync must never write, even if they differ: my state or per-card
# identity (the card keeps its own; the image copy is a fresh-card default).
NEVER_SYNC = {
    "/root/.ssh/authorized_keys": "my SSH keys on the card win",
    "/etc/nzeus-rootfs-release": "build stamp of the image; the card's own stays",
    "/etc/timezone": "the card's setting wins",
    "/etc/localtime": "the card's setting wins",
    "/etc/nzeus/usbnet-at-boot": "my switch (deleting it turns the USB network off)",
    "/etc/nzeus/autostart-modern": "my switch (Deck autostart)",
    "/etc/hostname": "the card's setting wins",
}
# Start-up scripts: changing them changes the next boot. Lab rule 6:
# start-up changes only while I watch a restart.
STARTUP_PREFIXES = ("/etc/init.d/", "/etc/inittab", "/etc/fstab", "/usr/local/sbin/nzeus-",
                    "/etc/sysctl.d/", "/etc/default/")


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def files_of(prov):
    return {e["path"]: e for e in prov["managed"] if e.get("in_image")}


def collect(prov):
    paths = [p for p, e in sorted(files_of(prov).items()) if e.get("type") != "symlink"]
    print("# READ-ONLY. Run on the console's SD Debian, save the output as LIVE.txt.")
    print("# It only reads files (sha256sum/stat); it changes nothing.")
    print("for f in \\")
    for p in paths:
        print("  '%s' \\" % p)
    print("  ; do if test -f \"$f\"; then printf '%s %s %s\\n' \"$(sha256sum < \"$f\" | cut -d' ' -f1)\" "
          "\"$(stat -c %a \"$f\")\" \"$f\"; else echo \"MISSING - $f\"; fi; done")


def parse_live(path):
    live = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3 and parts[0] == "MISSING":
                live[parts[2]] = None
            elif len(parts) == 3:
                live[parts[2]] = {"sha256": parts[0], "mode": parts[1]}
    return live


def classify(img, live_entry):
    if live_entry is None:
        return "missing-on-live"
    if "sha256" in live_entry:
        return "same" if live_entry["sha256"] == img.get("sha256") else "differs"
    if "sha256_prefix" in live_entry:
        return "same" if img.get("sha256", "").startswith(live_entry["sha256_prefix"]) else "differs"
    return "unknown"


def changed_since(e, since):
    """docs mode: did the repo source change after the live image was installed?"""
    d = e.get("source_last_commit_date")
    if not since or not d:
        return ""
    from datetime import datetime
    return " (source changed %s, after the live image)" % d[:16] if datetime.fromisoformat(d) > datetime.fromisoformat(since)         else " (source unchanged since the live image)"


def rows(prov, live, unknown_default):
    out = []
    for p, e in sorted(files_of(prov).items()):
        if e.get("type") == "symlink":
            state = "link (checked by target: %s)" % e.get("link")
            out.append((p, e, state, None))
            continue
        if p in live:
            state = classify(e, live[p])
        else:
            state = unknown_default
        out.append((p, e, state, live.get(p)))
    return out


def action(p, state):
    if p in NEVER_SYNC:
        return "NEVER (" + NEVER_SYNC[p] + ")"
    if state in ("same",) or state.startswith("link"):
        return "nothing"
    if state == "unknown-live-hash":
        return "read live hash first (collect)"
    startup = p.startswith(STARTUP_PREFIXES)
    a = "copy (backup old -> /data/nzeus/sync-backup-DATE, readback)"
    if startup:
        a += "; START-UP FILE: only with me watching a restart"
    if p == "/usr/local/bin/nzeus-modern":
        a += "; app: use the signed updater + 60-s watcher"
    return a


def package_diff(prov, live_pk):
    img = {p["name"]: p["version"] for p in prov["packages"]}
    only_img = sorted(set(img) - set(live_pk))
    only_live = sorted(set(live_pk) - set(img))
    ver = sorted(n for n in set(img) & set(live_pk) if img[n] != live_pk[n]["version"])
    return img, only_img, only_live, ver


def write(text, out):
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("written:", out)
    else:
        print(text)


def report(prov, live, title, unknown_default, live_pk=None, extra=None, since=None):
    L = []
    L.append("# " + title)
    L.append("")
    L.append("DRY RUN ONLY. Nothing here was executed. The console was not contacted.")
    L.append("")
    L.append("Image: `%s` sha256 `%s`, built %s from commit `%s`." % (
        prov["image"]["file"], prov["image"]["sha256"], prov["build"]["built_utc"], prov["build"]["repo_commit"]))
    L.append("")
    if extra:
        L += extra
    rs = rows(prov, live, unknown_default)
    counts = {}
    for _, _, s, _ in rs:
        k = "link" if s.startswith("link") else s
        counts[k] = counts.get(k, 0) + 1
    L.append("## Managed files (%d): %s" % (len(rs), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    L.append("")
    L.append("| path | image sha256 | live | state | sync action |")
    L.append("|---|---|---|---|---|")
    for p, e, s, lv in rs:
        if lv is None:
            lvs = "-"
        elif "sha256" in lv:
            lvs = lv["sha256"][:12]
        elif "sha256_prefix" in lv:
            lvs = lv["sha256_prefix"][:12] + " (prefix)"
        else:
            lvs = "?"
        hint = changed_since(e, since) if s == "unknown-live-hash" else ""
        L.append("| `%s` | %s | %s | %s%s | %s |" % (p, (e.get("sha256") or "-")[:12], lvs, s, hint, action(p, s)))
    if live_pk is not None:
        img, only_img, only_live, ver = package_diff(prov, live_pk)
        L.append("")
        L.append("## Packages (not part of a file sync; need apt on the nofpu kernel)")
        L.append("")
        L.append("Image %d, live (docs) %d." % (len(img), len(live_pk)))
        L.append("")
        L.append("- In the image, not recorded on the live card (%d): %s" % (len(only_img), ", ".join(only_img) or "none"))
        L.append("- Recorded on the live card, not in the image (%d): %s" % (len(only_live), ", ".join(only_live) or "none"))
        L.append("- Different versions (%d): %s" % (len(ver), ", ".join(
            "%s %s -> %s" % (n, live_pk[n]["version"], img[n]) for n in ver) or "none"))
    L.append("")
    L.append("## How a real sync would run (NOT done here)")
    L.append("")
    L.append("1. I connect the console; fresh read-only `collect` output replaces the docs-based column.")
    L.append("2. Re-run `plan`; only `differs`/`missing-on-live` rows with a copy action are candidates.")
    L.append("3. I approve the list. Old copies are saved to `/data/nzeus/sync-backup-DATE` first;")
    L.append("   every copied file is read back by hash; nothing in `/data` is changed otherwise.")
    L.append("4. Start-up files only while I watch a restart (lab rule 6); the app")
    L.append("   via the signed updater with its 60-second watcher. Never a raw device write.")
    return "\n".join(L) + "\n"


def main():
    a = sys.argv[1:]
    out = None
    if "--out" in a:
        i = a.index("--out")
        out = a[i + 1]
        del a[i:i + 2]
    if len(a) == 2 and a[0] == "collect":
        # the console's shell chokes on Windows CR LF line ends ("word unexpected"):
        # always print plain LF, whatever system this runs on
        sys.stdout.reconfigure(newline=chr(10))
        collect(load(a[1]))
    elif len(a) == 3 and a[0] == "plan":
        prov = load(a[1])
        live = parse_live(a[2])
        write(report(prov, live, "Managed-file sync plan (live hashes)", "unknown-live-hash"), out)
    elif len(a) == 3 and a[0] == "docs":
        prov = load(a[1])
        known = load(a[2])
        extra = ["Live state is taken from the repo docs (" + a[2].replace("\\", "/").split("/")[-1] +
                 "), not read from the console. Live root image: `%s`." % known["root_image_installed"]["sha256"], "",
                 "Added on the live card by hand after that image (docs):", ""]
        for x in known["added_by_hand_after_image"]:
            extra.append("- %s (%s)" % (x["what"], x["source"]))
        extra.append("")
        write(report(prov, known["managed"], "What differs: new image vs the live card (docs)",
                     "unknown-live-hash", known["packages"], extra,
                     known["root_image_installed"].get("installed_utc")), out)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
