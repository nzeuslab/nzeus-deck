"""Run the actual overlay on disposable image roots, never on a card."""
import os
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]


class Reuse(unittest.TestCase):
    def test_generic_flag_survives_builder_sudo_handoff(self):
        with tempfile.TemporaryDirectory(prefix='nzeus-sudo-fixture-') as folder:
            root = Path(folder)
            bindir = root / 'bin'
            bindir.mkdir()
            (bindir / 'id').write_text('#!/bin/sh\necho 1000\n')
            (bindir / 'sudo').write_text('''#!/usr/bin/python3
import json,os,sys
from pathlib import Path
Path(os.environ['FAKE_SUDO_LOG']).write_text(json.dumps(sys.argv[1:]))
''')
            for executable in bindir.iterdir():
                executable.chmod(0o755)
            log = root / 'sudo-args.json'
            env = dict(os.environ, PATH=str(bindir) + ':' + os.environ['PATH'],
                       NZ_NO_AUTHORIZED_KEYS='1', NZ_AUTHORIZED_KEYS='', FAKE_SUDO_LOG=str(log))
            env.pop('SUDO_USER', None)
            result = subprocess.run(['bash', str(HERE / 'build-debian.sh'), str(root / 'work'), '--reuse'],
                                    env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('NZ_NO_AUTHORIZED_KEYS=1', json.loads(log.read_text()))

    def test_provenance_distinguishes_optional_owner_access(self):
        spec = importlib.util.spec_from_file_location('provenance', HERE / 'provenance.py')
        provenance = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(provenance)
        with tempfile.TemporaryDirectory() as folder:
            manifest = Path(folder) / 'manifest'
            for value in ('yes', 'no'):
                manifest.write_text('authorized_keys=' + value + '\n')
                actual = provenance.parse_manifest(manifest)
                self.assertEqual(actual.get('authorized_keys'), value)
                paths = [p for p, _, _ in provenance.mapping(actual['authorized_keys'] == 'yes')]
                self.assertEqual('/root/.ssh/authorized_keys' in paths, value == 'yes')

    def test_generic_image_removes_inherited_owner_key_and_obsolete_helper(self):
        with tempfile.TemporaryDirectory(prefix='nzeus-image-reuse-') as folder:
            root = Path(folder)
            key = root / 'root/.ssh/authorized_keys'
            key.parent.mkdir(parents=True)
            key.write_text('inherited-owner-access')
            stub = root / 'usr/local/lib/nzeus/deck-bin/nzeus-claude-access.sh'
            stub.parent.mkdir(parents=True)
            stub.write_text('obsolete stub')
            other = stub.parent / 'owner-local-tool'
            other.write_text('preserve unrelated tool')
            env = dict(os.environ, NZ_NO_AUTHORIZED_KEYS='1', NZ_AUTHORIZED_KEYS='/does/not/exist')
            result = subprocess.run(['sh', str(HERE / 'apply-overlay.sh'), str(root)],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(key.exists(), 'generic fresh cards must not inherit owner access')
            self.assertFalse(stub.exists(), 'reuse must not ship the removed Claude-access stub')
            self.assertEqual(other.read_text(), 'preserve unrelated tool')
            self.assertTrue((stub.parent / 'nzeus-claude-connect.sh').is_file())


if __name__ == '__main__':
    unittest.main(verbosity=2)
