"""PC-only helper tests: fake update targets, signed fixtures and loopback HTTP.

Run: python3 linux-port/deck-modern/tests/test_helpers.py
Never calls SSH, a real updater target, board uploader or WhatsApp bridge.
"""
import hashlib
import hmac
import http.server
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

HELPERS = Path(__file__).resolve().parents[1]
REPO = HELPERS.parents[1]
spec = importlib.util.spec_from_file_location("publisher", REPO / "tools/publish_update.py")
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)
KEY = bytes(range(32))                         # public test fixture, never a console key
ELF = b"\x7fELF\x01\x01" + b"\x00" * 12 + b"\x08\x00"


class Helpers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="deck-helper-test-")
        self.root = Path(self.temp.name)
        self.server = self.root / "server"
        (self.server / "files").mkdir(parents=True)
        self.update = self.root / "update"
        self.update.mkdir()
        (self.update / "key").write_text(KEY.hex())
        (self.update / "server").write_text("http://fixture")
        self.binary = self.root / "bin/nzeus-modern"
        self.binary.parent.mkdir()
        self.binary.write_bytes(ELF + b"old")
        self.deckbin = self.root / "deck-bin"
        self.deckbin.mkdir()
        self.pyex = self.root / "python-examples"       # (never the real /usr/local/lib)
        self.emoji = self.root / "deck/emoji"
        self.fetch = self.root / "fetch"
        self.fetch.write_text('#!/bin/sh\nfor arg do prev=$last; last=$arg; done\n'
                              'cp "$FIXTURE_SERVER/${prev#http://fixture/}" "$last"\n')
        self.fetch.chmod(0o755)
        self.env = dict(os.environ, NZ_UPD_DIR=str(self.update), NZ_UPD_BIN=str(self.binary),
                        NZ_UPD_DECKBIN=str(self.deckbin), NZ_UPD_FETCH=str(self.fetch),
                        NZ_UPD_PYEX=str(self.pyex), NZ_UPD_EMOJI=str(self.emoji),
                        NZ_UPD_LOCK=str(self.root / "update.lock"),
                        FIXTURE_SERVER=str(self.server))

    def tearDown(self):
        self.temp.cleanup()

    def run_update(self, command):
        return subprocess.run(["dash", str(HELPERS / "nzeus-update.sh"), command],
                              env=self.env, capture_output=True, text=True, timeout=20)

    def manifest(self, files, system="modern", version="test-version"):
        lines = ["NZEUS-UPDATE 1", f"system {system}", f"version {version}", "notes fixture"]
        for name, data in files.items():
            sha = hashlib.sha256(data).hexdigest()
            (self.server / "files" / sha).write_bytes(data)
            lines.append(f"file {sha} {len(data)} {name}")
        body = ("\n".join(lines) + "\n").encode()
        (self.server / "manifest.txt").write_bytes(body)
        (self.server / "manifest.sig").write_text(hmac.new(KEY, body, hashlib.sha256).hexdigest())

    def test_required_inventory_and_posix_syntax(self):
        names = (HELPERS / "required-files.txt").read_text().splitlines()
        self.assertEqual(len(names), len(set(names)))
        self.assertGreaterEqual(len(names), 20)
        for name in names:
            path = HELPERS / name
            self.assertTrue(path.is_file(), name)
            self.assertNotIn(b"\r", path.read_bytes(), name)
            if path.read_bytes().startswith(b"#!/usr/bin/python3"):
                compile(path.read_text(), str(path), 'exec')
            else:
                subprocess.run(["dash", "-n", str(path)], check=True)

    def test_install_and_rollback_preserve_originals(self):
        (self.deckbin / "python").write_text("old helper")
        self.manifest({"nzeus-modern": ELF + b"new", "deck-bin/python": b"new helper",
                       "deck-bin/new-helper": b"new file"})
        result = self.run_update("install")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.binary.read_bytes(), ELF + b"new")
        self.assertTrue((self.update / "pending").exists())
        result = self.run_update("rollback")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.binary.read_bytes(), ELF + b"old")
        self.assertEqual((self.deckbin / "python").read_text(), "old helper")
        self.assertFalse((self.deckbin / "new-helper").exists())
        self.assertFalse((self.update / "pending").exists())

    def test_examples_and_emoji_install_as_data_and_roll_back(self):
        self.manifest({"nzeus-modern": ELF + b"new", "deck-bin/python": b"helper",
                       "python-examples/hello.py": b"print('hi')\n",
                       "emoji/1f600.bin": b"\x05\x40\r\npicture", "emoji/index.txt": b"1f600\n"})
        result = self.run_update("install")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.pyex / "hello.py").read_bytes(), b"print('hi')\n")
        self.assertEqual((self.emoji / "1f600.bin").read_bytes(), b"\x05\x40\r\npicture")
        self.assertEqual((self.emoji / "index.txt").read_bytes(), b"1f600\n")
        for data in (self.pyex / "hello.py", self.emoji / "1f600.bin", self.emoji / "index.txt"):
            self.assertEqual(data.stat().st_mode & 0o777, 0o644, data)
        self.assertEqual((self.deckbin / "python").stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.binary.stat().st_mode & 0o777, 0o755)
        result = self.run_update("rollback")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.pyex / "hello.py").exists())
        self.assertFalse((self.emoji / "1f600.bin").exists())
        self.assertEqual(self.binary.read_bytes(), ELF + b"old")

    def test_reject_unsigned_stock_traversal_and_wrong_architecture(self):
        bad_paths = ("deck-bin/../outside", "python-examples/run.sh", "python-examples/../up.py",
                     "python-examples/.hidden.py", "python-examples/sub/x.py", "emoji/../up.bin",
                     "emoji/sub/x.bin", "emoji/.index", "examples/hello.py", "usr/local/bin/x")
        cases = ["signature", "stock", "architecture"] + ["path " + p for p in bad_paths]
        for case in cases:
            with self.subTest(case=case):
                files = {"nzeus-modern": ELF + b"new"}
                if case.startswith("path "): files[case[5:]] = b"bad"
                if case == "architecture": files["nzeus-modern"] = b"not MIPS"
                self.manifest(files, system="stock" if case == "stock" else "modern")
                if case == "signature": (self.server / "manifest.sig").write_text("0" * 64)
                result = self.run_update("install")
                self.assertNotEqual(result.returncode, 0, case)
                self.assertEqual(self.binary.read_bytes(), ELF + b"old")
                self.assertFalse((self.update / "pending").exists())
                self.assertFalse(self.pyex.exists() or self.emoji.exists())

    def test_older_signed_update_is_refused(self):
        # A replayed old (but correctly signed) update must not roll the console back.
        (self.update / "installed").write_text("2026-11-20 10:00\n")
        self.manifest({"nzeus-modern": ELF + b"older"}, version="2026-11-19 09:00")
        for command in ("check", "install"):
            result = self.run_update(command)
            self.assertNotEqual(result.returncode, 0, command)
            self.assertIn("OLDER", result.stdout, command)
        self.assertEqual(self.binary.read_bytes(), ELF + b"old")
        self.assertFalse((self.update / "pending").exists())
        # the owner can still go back on purpose (over SSH)
        self.env.update(NZ_UPD_ALLOW_OLDER="1")
        result = self.run_update("install")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.binary.read_bytes(), ELF + b"older")

    def test_newer_and_same_version_updates_install(self):
        (self.update / "installed").write_text("2026-11-20 10:00\n")
        for version in ("2026-11-20 10:00", "2026-11-21 08:30"):
            with self.subTest(version=version):
                self.manifest({"nzeus-modern": ELF + version.encode()}, version=version)
                result = self.run_update("install")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                (self.update / "pending").unlink()

    def test_damaged_download_changes_nothing(self):
        data = ELF + b"new"
        self.manifest({"nzeus-modern": data, "deck-bin/python": b"helper"})
        (self.server / "files" / hashlib.sha256(data).hexdigest()).write_bytes(b"damaged")
        self.assertNotEqual(self.run_update("install").returncode, 0)
        self.assertEqual(self.binary.read_bytes(), ELF + b"old")
        self.assertFalse((self.deckbin / "python").exists())

    def test_pending_update_cannot_be_overwritten(self):
        self.manifest({"nzeus-modern": ELF + b"new"})
        (self.update / "pending").write_text("earlier")
        self.assertNotEqual(self.run_update("install").returncode, 0)
        self.assertEqual((self.update / "pending").read_text(), "earlier")

    def test_missing_backup_keeps_pending(self):
        (self.update / "pending").write_text("earlier")
        self.assertNotEqual(self.run_update("rollback").returncode, 0)
        self.assertTrue((self.update / "pending").exists())

    def test_concurrent_check_cannot_replace_install_downloads(self):
        self.manifest({"nzeus-modern": ELF + b"new"})
        self.fetch.write_text('#!/bin/sh\ntouch "$FETCH_ENTERED"\n'
                              'while test ! -f "$FETCH_RELEASE"; do sleep 0.05; done\n'
                              'for arg do prev=$last; last=$arg; done\n'
                              'cp "$FIXTURE_SERVER/${prev#http://fixture/}" "$last"\n')
        entered, release = self.root / "entered", self.root / "release"
        self.env.update(FETCH_ENTERED=str(entered), FETCH_RELEASE=str(release))
        first = subprocess.Popen(["dash", str(HELPERS / "nzeus-update.sh"), "install"],
                                 env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not entered.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(entered.exists(), "first installer did not reach its download")
            second = self.run_update("check")
            self.assertNotEqual(second.returncode, 0)
            self.assertIn("another updater is active", second.stdout)
            release.touch()
            stdout, stderr = first.communicate(timeout=15)
            self.assertEqual(first.returncode, 0, stdout + stderr)
            self.assertEqual(self.binary.read_bytes(), ELF + b"new")
            self.assertFalse(Path(self.env["NZ_UPD_LOCK"]).exists())
        finally:
            release.touch()
            if first.poll() is None:
                first.kill()
            first.communicate(timeout=5)

    def test_install_failure_rolls_back_without_lock_recursion(self):
        blocked = self.deckbin / "blocked"
        blocked.mkdir()                           # cannot replace a directory with a helper
        self.manifest({"nzeus-modern": ELF + b"new", "deck-bin/blocked": b"helper"})
        result = self.run_update("install")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.binary.read_bytes(), ELF + b"old")
        # The obstructing directory also prevents a full rollback; keep recovery state.
        self.assertTrue((self.update / "pending").exists())
        self.assertIn("rollback incomplete", result.stdout)
        self.assertFalse(Path(self.env["NZ_UPD_LOCK"]).exists())

    def test_restart_watcher_commits_a_running_deck(self):
        self.manifest({"nzeus-modern": ELF + b"new"})
        self.assertEqual(self.run_update("install").returncode, 0)
        deck = self.root / "deck"
        deck.write_text('#!/bin/sh\ncase "$1" in status) echo "running (pid 123)" ;; esac\nexit 0\n')
        deck.chmod(0o755)
        self.env.update(NZ_UPD_DECK=str(deck), NZ_UPD_WATCH="0")
        result = self.run_update("restart-now")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.update / "pending").exists())
        self.assertEqual((self.update / "installed").read_text().strip(), "test-version")
        self.assertEqual(self.binary.read_bytes(), ELF + b"new")

    def test_restart_watcher_restores_a_deck_that_cannot_start(self):
        self.manifest({"nzeus-modern": ELF + b"new"})
        self.assertEqual(self.run_update("install").returncode, 0)
        deck = self.root / "deck"
        deck.write_text('#!/bin/sh\ncase "$1" in start) exit 1 ;; esac\nexit 0\n')
        deck.chmod(0o755)
        self.env.update(NZ_UPD_DECK=str(deck), NZ_UPD_WATCH="0")
        result = self.run_update("restart-now")
        self.assertIn("the last update was undone", result.stdout)
        self.assertFalse((self.update / "pending").exists())
        self.assertEqual(self.binary.read_bytes(), ELF + b"old")

    def test_failed_restore_keeps_pending_and_backup(self):
        back = self.update / "previous"
        (back / "files/deck-bin").mkdir(parents=True)
        (back / "list").write_text("old deck-bin/python\n")
        (back / "files/deck-bin/python").write_text("original")
        (self.update / "pending").write_text("earlier")
        blocked = self.root / "blocked"
        blocked.write_text("a file cannot be a destination folder")
        self.env["NZ_UPD_DECKBIN"] = str(blocked)
        self.assertNotEqual(self.run_update("rollback").returncode, 0)
        self.assertTrue((self.update / "pending").exists())
        self.assertEqual((back / "files/deck-bin/python").read_text(), "original")

    def test_publisher_refuses_missing_helpers(self):
        helper_copy = self.root / "helpers"
        shutil.copytree(HELPERS, helper_copy)
        (helper_copy / "python").unlink()
        saved = publisher.MODERN_BIN, publisher.DECK_MODERN
        publisher.MODERN_BIN, publisher.DECK_MODERN = str(self.binary), str(helper_copy)
        try:
            with self.assertRaises(SystemExit):
                publisher.build_modern(str(self.root / "output"), "test", KEY)
            self.assertFalse((self.root / "output").exists())
        finally:
            publisher.MODERN_BIN, publisher.DECK_MODERN = saved

    def publish(self, emoji=False, emoji_dir=None, helpers=HELPERS):
        """Run the real publisher (fixture binary + key) into the fixture server."""
        saved = publisher.MODERN_BIN, publisher.DECK_MODERN, publisher.EMOJI_DIR
        publisher.MODERN_BIN, publisher.DECK_MODERN = str(self.binary), str(helpers)
        publisher.EMOJI_DIR = str(emoji_dir) if emoji_dir else None
        try:
            shutil.rmtree(self.server)
            publisher.build_modern(str(self.server), "fixture", KEY, emoji)
        finally:
            publisher.MODERN_BIN, publisher.DECK_MODERN, publisher.EMOJI_DIR = saved
        lines = (self.server / "manifest.txt").read_text().splitlines()
        return {l.split()[3]: l.split()[1] for l in lines if l.startswith("file ")}

    def fake_emoji_pack(self):
        pack = self.root / "emoji-pack"
        pack.mkdir()
        (pack / "1f600.bin").write_bytes(b"\x05\x40\x00\x02smile\r")
        (pack / "index.txt").write_text("1f600\n")
        (pack / "LICENSE.txt").write_text("Twemoji CC-BY 4.0 (fixture)\n")
        (pack / "notes.tmp").write_text("not part of the pack")
        return pack

    def test_publisher_lists_helpers_bt_examples_and_no_emoji_by_default(self):
        listed = self.publish()
        names = (HELPERS / "required-files.txt").read_text().split()
        for name in names:
            self.assertIn("deck-bin/" + name, listed)
        self.assertIn("deck-bin/wifi-join-core.sh", listed)
        self.assertNotIn("deck-bin/nzeus-claude-access.sh", listed)
        bt_sha = (HELPERS / "bt/SHA256SUMS").read_text().split()[0]
        self.assertEqual(listed["deck-bin/nzeus-bt.bin"], bt_sha)
        examples = sorted(p.name for p in (HELPERS / "python-examples").glob("*.py"))
        self.assertGreaterEqual(len(examples), 7)
        self.assertEqual(sorted(p[len("python-examples/"):] for p in listed if p.startswith("python-examples/")), examples)
        self.assertFalse([p for p in listed if p.startswith("emoji/")])
        self.assertEqual(len(listed), 1 + len(names) + 1 + len(examples))

    def test_publisher_emoji_only_when_asked_and_only_when_made(self):
        pack = self.fake_emoji_pack()
        listed = self.publish(emoji=True, emoji_dir=pack)
        self.assertEqual(sorted(p for p in listed if p.startswith("emoji/")),
                         ["emoji/1f600.bin", "emoji/LICENSE.txt", "emoji/index.txt"])
        with self.assertRaises(SystemExit):
            self.publish(emoji=True, emoji_dir=self.root / "no-pack")

    def test_publisher_refuses_a_changed_bluetooth_program(self):
        helper_copy = self.root / "helpers"
        shutil.copytree(HELPERS, helper_copy, ignore=shutil.ignore_patterns("build"))
        with open(helper_copy / "bt/nzeus-bt-mipsel", "ab") as f:
            f.write(b"x")
        with self.assertRaises(SystemExit):
            self.publish(helpers=helper_copy)

    def test_published_update_installs_on_the_console_side(self):
        pack = self.fake_emoji_pack()
        listed = self.publish(emoji=True, emoji_dir=pack)
        result = self.run_update("check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # (the fixture's nzeus-modern is already the published one: not counted)
        self.assertIn(f"FILES {len(listed) - 1}", result.stdout)
        result = self.run_update("install")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.deckbin / "nzeus-bt.bin").read_bytes(), (HELPERS / "bt/nzeus-bt-mipsel").read_bytes())
        self.assertEqual((self.deckbin / "wifi-join-core.sh").read_bytes(), (HELPERS / "wifi-join-core.sh").read_bytes())
        self.assertEqual((self.pyex / "hello.py").read_bytes(), (HELPERS / "python-examples/hello.py").read_bytes())
        self.assertEqual((self.emoji / "1f600.bin").read_bytes(), (pack / "1f600.bin").read_bytes())
        self.assertFalse((self.emoji / "notes.tmp").exists())
        result = self.run_update("check")
        self.assertIn("FILES 0", result.stdout)

    def test_fetch_limits_unknown_length_and_keeps_old_file(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()                      # deliberately no Content-Length
                self.wfile.write(b"x" * (101 if self.path == "/large" else 30))

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}"
            args = ["dash", str(HELPERS / "nzeus-fetch"), "--max-size", "100"]
            result = subprocess.run(args + [url + "/small"], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b"x" * 30)
            result = subprocess.run(args + [url + "/large"], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            output = self.root / "download"
            output.write_bytes(b"original")
            result = subprocess.run(args + [url + "/large", str(output)], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"original")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
