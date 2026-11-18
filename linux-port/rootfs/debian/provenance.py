#!/usr/bin/env python3
"""provenance.py IMAGE OUT.json [--root ROOT] - record what is inside a built
nzroot image (rootfs.ext4) so a later managed-file SYNC knows exactly what a
fresh card would get.

Runs in WSL (needs debugfs from e2fsprogs; run as root so /root/.ssh can be
read from the root folder cross-check). It only READS the image file and the
repo. It never writes a card, never talks to the console (lab rule 8).

What it writes (JSON):
  image      sha256, bytes, label/uuid line from IMAGE.info
  build      built_utc, Debian suite/arch/mirror/Release date, repo commit,
             tracked diff state (copied from manifest/debian-rootfs-manifest.txt)
  packages   every installed package (name, version, arch) read from the
             image's own /var/lib/dpkg/status
  managed    every NZEUS-managed path: sha256, bytes, mode, where it came from
             (repo file + the last commit that changed it, or "generated"),
             and whether the image copy is byte-identical to that source
  unmapped   files under the NZEUS folders that this list does not explain
             (should be empty; anything here is a surprise to look at)

Files are read straight out of the image with debugfs (no mount, no loop
device), so the hashes describe the image, not the root folder it came from.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OVL = os.path.join(HERE, "overlay")
# Debian's own entries inside the NZEUS folders (base-files: /usr/local/man -> share/man)
DEBIAN_OWN = {"/usr/local/man"}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw).stdout


def git(*args):
    return run(["git", "-c", "safe.directory=" + REPO, "-C", REPO] + list(args)).decode().strip()


def last_commit(repo_path):
    """the newest commit that changed this repo file ('' if untracked)"""
    out = git("log", "-1", "--format=%H", "--", repo_path)
    return out


def tracked(repo_path):
    return git("ls-files", "--", repo_path) != ""


def sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


class Image:
    """read-only access to an ext4 image file through debugfs"""

    def __init__(self, path):
        self.path = path

    def cat(self, p):
        return run(["debugfs", "-R", "cat " + p, self.path])

    def stat(self, p):
        out = run(["debugfs", "-R", "stat " + p, self.path]).decode("utf-8", "replace")
        if "File not found" in out or not out.strip():
            return None
        info = {}
        m = re.search(r"Type:\s+(\w+)\s+Mode:\s+(\d+)", out)
        if m:
            info["type"], info["mode"] = m.group(1), m.group(2)
        m = re.search(r"User:\s+(\d+)\s+Group:\s+(\d+)\s+.*Size:\s+(\d+)", out)
        if m:
            info["uid"], info["gid"], info["bytes"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
        m = re.search(r'Fast link dest: "([^"]*)"', out)
        if m:
            info["link"] = m.group(1)
        return info

    def walk(self, top):
        """every regular file / link below top (recursive), as image paths"""
        out = run(["debugfs", "-R", "ls -p " + top, self.path]).decode("utf-8", "replace")
        found = []
        for line in out.splitlines():
            # /inode/mode/uid/gid/name/size/
            parts = line.strip().strip("/").split("/")
            if len(parts) < 5:
                continue
            mode, name = parts[1], parts[4]
            if name in (".", ".."):
                continue
            p = top.rstrip("/") + "/" + name
            if (int(mode, 8) & 0o170000) == 0o040000:   # directory (debugfs prints e.g. 040755)
                found += self.walk(p)
            else:
                found.append(p)
        return found


def mapping(include_owner_key=True):
    """(image path, repo source path or None, kind) for every NZEUS-managed file.
    Mirrors apply-overlay.sh + build-debian.sh; the 'unmapped' check below
    catches any file this list forgets."""
    m = []
    for dp, dn, fn in os.walk(OVL):
        for f in fn:
            src = os.path.join(dp, f)
            rel = os.path.relpath(src, OVL).replace(os.sep, "/")
            m.append(("/" + rel, "linux-port/rootfs/debian/overlay/" + rel, "overlay"))
    for s in ("usbnet-up.sh", "usbnet-down.sh", "usbnet-confirm.sh"):
        m.append(("/usr/local/lib/nzeus/usbnet/" + s, "linux-port/usbnet/" + s, "copy"))
    for s in ("usb-host-trial.sh", "usb-host-confirm.sh", "usb-host-off.sh", "usb-host-status.sh",
              "wifi-up.sh", "wifi-down.sh", "udhcpc.script", "wifi.conf.example", "fn-hold.py"):
        m.append(("/usr/local/lib/nzeus/usbhost/" + s, "linux-port/usbhost/" + s, "copy"))
    for b in ("nz-cdeject", "usb-regs"):
        m.append(("/usr/local/sbin/" + b, "linux-port/usbhost/%s-mipsel" % b, "copy"))
    with open(os.path.join(REPO, "linux-port/deck-modern/required-files.txt")) as f:
        for name in f.read().split():
            m.append(("/usr/local/lib/nzeus/deck-bin/" + name, "linux-port/deck-modern/" + name, "copy"))
    m.append(("/usr/local/lib/nzeus/deck-bin/SHA256SUMS", None, "generated: sha256 list of the Deck helpers"))
    m.append(("/usr/local/lib/nzeus/deck-bin/nzeus-bt.bin", "linux-port/deck-modern/bt/nzeus-bt-mipsel", "copy"))
    pyex = os.path.join(REPO, "linux-port/deck-modern/python-examples")
    for name in sorted(os.listdir(pyex)):
        if name.endswith(".py"):
            m.append(("/usr/local/lib/nzeus/python-examples/" + name,
                      "linux-port/deck-modern/python-examples/" + name, "copy"))
    emoji = os.path.join(REPO, "linux-port/deck-modern/build/emoji")     # tools/make_emoji.py (not in git)
    if os.path.isdir(emoji):
        for name in sorted(os.listdir(emoji)):
            if name.endswith(".bin") or name in ("index.txt", "LICENSE.txt"):
                m.append(("/usr/local/lib/nzeus/emoji/" + name,
                          "linux-port/deck-modern/build/emoji/" + name, "copy"))
    m.append(("/usr/local/sbin/dropbearmulti-modern", "linux-port/usbnet/dropbear/dropbearmulti-modern-mipsel", "copy"))
    m.append(("/usr/bin/scp", None, "generated: link to ../local/sbin/dropbearmulti-modern"))
    m.append(("/usr/local/bin/nzeus-modern", "apps/frontend-modern/mips/nzeus-modern",
              "built: build_nzeus_modern.sh from apps/nzeus-gui + apps/frontend-modern"))
    m.append(("/usr/local/bin/python", None, "generated: link to /usr/bin/python3"))
    m.append(("/etc/nzeus-rootfs-release", None, "generated by apply-overlay.sh (build time stamp)"))
    m.append(("/etc/timezone", None, "generated by build-debian.sh (UTC or NZ_TIMEZONE)"))
    m.append(("/etc/localtime", None, "generated: link to the chosen zone file"))
    if include_owner_key:
        m.append(("/root/.ssh/authorized_keys", None,
                  "PUBLIC key file from WSL ~/r36s/keys/authorized_keys (never in git; only its hash is recorded)"))
    return m


def parse_manifest(path):
    keep = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("installed packages"):
                break
            for key in ("built_utc", "repo_commit", "repo_tracked_changes", "repo_tracked_diff_sha256",
                        "deck_helpers_manifest_sha256", "nzeus_modern_sha256", "tree_bytes", "tree_files",
                        "sha256", "uuid", "label", "used_bytes", "allocated_bytes", "bytes", "authorized_keys"):
                if line.startswith(key + "="):
                    keep[key] = line.split("=", 1)[1]
            if line.startswith("suite="):
                keep["debian"] = line
            if line.startswith("debian_release_"):
                keep["debian_release"] = line[len("debian_release_"):]
            if line.startswith("rcS order:"):
                keep["rcS_order"] = line.split(":", 1)[1].strip()
    return keep


def packages(img):
    status = img.cat("/var/lib/dpkg/status").decode("utf-8", "replace")
    pk = []
    for block in status.split("\n\n"):
        f = {}
        for line in block.splitlines():
            if ":" in line and not line.startswith(" "):
                k, v = line.split(":", 1)
                f[k] = v.strip()
        if "install ok installed" in f.get("Status", ""):
            pk.append({"name": f["Package"], "version": f.get("Version", ""), "arch": f.get("Architecture", "")})
    return sorted(pk, key=lambda p: p["name"])


def main():
    a = sys.argv[1:]
    root = None
    if "--root" in a:
        i = a.index("--root")
        root = a[i + 1]
        del a[i:i + 2]
    if len(a) != 2:
        sys.exit(__doc__)
    image_path, out_path = a
    img = Image(image_path)
    man = parse_manifest(os.path.join(HERE, "manifest", "debian-rootfs-manifest.txt"))
    image_sha = sha_file(image_path)
    if man.get("sha256") and man["sha256"] != image_sha:
        sys.exit("provenance: the manifest describes another image (sha %s, this one %s)" % (man["sha256"], image_sha))
    build_commit = man.get("repo_commit", "")

    managed = []
    for ipath, src, kind in sorted(mapping(man.get("authorized_keys") == "yes")):
        st = img.stat(ipath)
        e = {"path": ipath, "kind": kind}
        if st is None:
            e["in_image"] = False
            managed.append(e)
            continue
        e["in_image"] = True
        e.update({k: st[k] for k in ("type", "mode", "uid", "gid", "bytes") if k in st})
        if st.get("type") == "symlink":
            e["link"] = st.get("link", "(long link)")
        else:
            e["sha256"] = hashlib.sha256(img.cat(ipath)).hexdigest()
        if src:
            e["source"] = src
            sp = os.path.join(REPO, src)
            if tracked(src):
                e["source_last_commit"] = last_commit(src)
                e["source_last_commit_date"] = git("log", "-1", "--format=%cI", "--", src)
            else:
                e["source_last_commit"] = None
                e["source_note"] = "untracked build output; built from repo commit " + build_commit
            if os.path.isfile(sp) and "sha256" in e:
                e["matches_source"] = sha_file(sp) == e["sha256"]
        e["build_commit"] = build_commit
        managed.append(e)

    # anything under the NZEUS folders that the list above does not explain
    known = {e["path"] for e in managed}
    unmapped = []
    for top in ("/usr/local", "/etc/nzeus", "/etc/init.d"):
        for p in img.walk(top):
            if top == "/etc/init.d" and not os.path.basename(p).startswith("nzeus"):
                continue
            if "/__pycache__/" in p:
                continue
            if p in DEBIAN_OWN:
                continue
            if p not in known:
                unmapped.append(p)

    # optional cross-check: the root folder the image was made from
    root_check = None
    if root:
        diffs = []
        for e in managed:
            if e.get("sha256"):
                rp = root + e["path"]
                if not os.path.isfile(rp) or sha_file(rp) != e["sha256"]:
                    diffs.append(e["path"])
        root_check = {"root": root, "differences": diffs}

    # The builder's "tracked changes" flag compares the whole checkout. Seen
    # from WSL, folders that are Windows junctions to D: (old card snapshots)
    # look deleted, so the flag can say "yes" although no image input changed.
    # List the changed paths and say whether any of them feeds the image.
    inputs = {e["source"] for e in managed if e.get("source")}
    input_dirs = ("apps/nzeus-gui/", "apps/frontend-modern/", "linux-port/rootfs/debian/overlay/",
                  "linux-port/rootfs/debian/apply-overlay.sh", "linux-port/rootfs/debian/build-debian.sh",
                  "linux-port/rootfs/debian/make-image.sh", "linux-port/rootfs/debian/packages.txt",
                  "linux-port/rootfs/debian/mke2fs.conf", "linux-port/deck-modern/", "build_nzeus_modern.sh")
    diff = []
    for line in git("diff", "--name-status", "HEAD").splitlines():
        st, _, p = line.partition("\t")
        feeds = p in inputs or any(p.startswith(d) or p == d for d in input_dirs)
        diff.append({"status": st, "path": p, "image_input": feeds})

    doc = {
        "what":"NZEUS SD Debian root image (nzroot) provenance",
        "written_by": "linux-port/rootfs/debian/provenance.py",
        "image": {
            "file": os.path.basename(image_path),
            "sha256": image_sha,
            "bytes": os.path.getsize(image_path),
            "label": man.get("label"),
            "uuid_before_region_tool": man.get("uuid"),
            "used_bytes": man.get("used_bytes"),
        },
        "build": {
            "built_utc": man.get("built_utc"),
            "debian": man.get("debian"),
            "debian_release_file": man.get("debian_release"),
            "repo_commit": build_commit,
            "repo_tracked_changes": man.get("repo_tracked_changes"),
            "repo_tracked_diff_sha256": man.get("repo_tracked_diff_sha256"),
            "deck_helpers_manifest_sha256": man.get("deck_helpers_manifest_sha256"),
            "nzeus_modern_sha256": man.get("nzeus_modern_sha256"),
            "rcS_order": man.get("rcS_order"),
            "kernel": "not in this image (the kernel lives in the stock FAT32 boot files)",
        },
        "tracked_diff_now": {
            "note": "git diff HEAD as seen when provenance ran (the manifest itself is rewritten by the build)",
            "files": diff,
            "any_image_input_changed": any(d["image_input"] for d in diff),
        },
        "packages": packages(img),
        "managed": managed,
        "unmapped": unmapped,
        "root_folder_check": root_check,
    }
    doc["counts"] = {
        "packages": len(doc["packages"]),
        "managed": len(managed),
        "managed_missing_from_image": sum(1 for e in managed if not e.get("in_image")),
        "managed_not_matching_source": sum(1 for e in managed if e.get("matches_source") is False),
        "unmapped": len(unmapped),
    }
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=False)
        f.write("\n")
    print("provenance: %s (%d packages, %d managed paths, %d missing, %d not matching source, %d unmapped)" % (
        out_path, doc["counts"]["packages"], doc["counts"]["managed"], doc["counts"]["managed_missing_from_image"],
        doc["counts"]["managed_not_matching_source"], doc["counts"]["unmapped"]))


if __name__ == "__main__":
    main()
