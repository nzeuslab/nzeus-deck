"""Exercise the real shell helper with a fake SSH executable, never real chats."""
import base64
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'nzeus-wa.sh'


class WhatsAppTransport(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='wa-transport-')
        self.root = Path(self.temp.name)
        self.key = self.root / 'fixture-key'
        self.key.write_text('Not a real private key\n')
        self.calls = self.root / 'calls'
        self.client = self.root / 'fake-client'
        self.client.write_text('''#!/usr/bin/python3
import json,os,sys,time
from pathlib import Path
host=sys.argv[-2].split('@',1)[1]
with open(os.environ['FAKE_CALLS'],'a') as f: f.write(host+'\\n')
answer=json.loads(os.environ['FAKE_FLOW'])[host]
if answer.get('hold'): time.sleep(30)
sys.stdout.write(answer.get('stdout',''))
sys.stderr.write(answer.get('stderr',''))
raise SystemExit(answer.get('code',0))
''')
        self.client.chmod(0o755)
        self.env = dict(os.environ, NZ_ROOT=str(self.root / 'deck'),
                        NZ_WA_KEY=str(self.key), NZ_WA_HOST='192.0.2.145',
                        NZ_WA_HOST_AWAY='', NZ_WA_USER='fixture', NZ_WA_PORT='22',
                        NZ_GW='192.0.2.1', NZ_DBMULTI=str(self.client),
                        TMPDIR=str(self.root), FAKE_CALLS=str(self.calls))

    def tearDown(self):
        self.temp.cleanup()

    def run_helper(self, args=('status',), first=None, second=None):
        flow = {'192.0.2.145': first or {}}
        if second is not None:
            self.env['NZ_WA_HOST_AWAY'] = 'away.example'
            flow['away.example'] = second
        self.env['FAKE_FLOW'] = json.dumps(flow)
        result = subprocess.run(['dash', str(HELPER), *args], env=self.env,
                                capture_output=True, timeout=10)
        self.assertEqual(list(self.root.glob('nzeus-wa-ssh.*')), [], 'SSH diagnostics must be removed')
        return result

    def test_rejected_key_explains_authorization_without_exposing_stderr(self):
        result = self.run_helper(first={'code': 255, 'stderr':
            "dbclient: Connection to fixture@pc:22 exited: No auth methods could be used.\nprivate-path\n"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: PC rejected the WhatsApp key', result.stdout)
        self.assertNotIn(b'private-path', result.stdout + result.stderr)

    def test_changed_host_key_has_its_own_message(self):
        result = self.run_helper(first={'code': 255, 'stderr': 'ED25519 host key mismatch for pc !\n'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: PC identity changed', result.stdout)

    def test_key_loading_failure_points_to_unlocking(self):
        result = self.run_helper(first={'code': 255, 'stderr': "Failed loading keyfile '/private/key'\n"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: Could not load the WhatsApp key', result.stdout)
        self.assertNotIn(b'/private/key', result.stdout + result.stderr)

    def test_network_failure_says_to_check_address_and_connection(self):
        result = self.run_helper(first={'code': 255, 'stderr': 'Connection refused\n'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: Could not reach the PC', result.stdout)

    def test_unknown_handshake_failure_does_not_claim_wrong_key(self):
        result = self.run_helper(first={'code': 255, 'stderr': 'Unexpected algorithm failure\n'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: Secure PC connection failed', result.stdout)
        self.assertNotIn(b'rejected', result.stdout)

    def test_success_keeps_utf8_protocol_bytes_and_discards_warnings(self):
        answer = 'state=ready\tme=مرحبا 😀\n'
        result = self.run_helper(first={'stdout': answer, 'stderr': 'first-use notice\n'})
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, answer.encode())
        self.assertEqual(result.stderr, b'')

    def test_read_falls_back_after_unreachable_home_address(self):
        result = self.run_helper(args=('chats',), first={'code': 255, 'stderr': 'Connection refused\n'},
                                 second={'stdout': 'fixture-chat\t0\t1\tName\tHello\n'})
        self.assertEqual(result.returncode, 0)
        self.assertTrue(result.stdout.startswith(b'fixture-chat\t'))
        self.assertEqual(self.calls.read_text().splitlines(), ['192.0.2.145', 'away.example'])

    def test_authentication_error_is_not_lost_behind_second_network_failure(self):
        result = self.run_helper(first={'code': 255, 'stderr': 'No auth methods could be used.\n'},
                                 second={'code': 255, 'stderr': 'Connection refused\n'})
        self.assertIn(b'error: PC rejected the WhatsApp key', result.stdout)

    def test_send_is_never_replayed_at_second_address(self):
        result = self.run_helper(args=('send', 'fixture-chat', 'aGVsbG8='),
                                 first={'code': 255, 'stderr': 'Connection refused\n'},
                                 second={'stdout': 'ok\n'})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls.read_text().splitlines(), ['192.0.2.145'])
        self.assertIn(b'error: Could not reach the PC', result.stdout)

    def test_photo_authentication_failure_preserves_existing_photo(self):
        picture = self.root / 'photo.jpg'
        picture.write_bytes(b'previous photo')
        result = self.run_helper(args=('photo', 'fixture-chat', 'message-id', str(picture)),
                                 first={'code': 255, 'stderr': 'No auth methods could be used.\n'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: PC rejected the WhatsApp key', result.stdout)
        self.assertEqual(picture.read_bytes(), b'previous photo')
        self.assertFalse(Path(str(picture) + '.tmp').exists())

    def test_photo_success_preserves_binary_framing(self):
        picture = self.root / 'photo.jpg'
        jpeg = b'\xff\xd8fixture\xff\xd9'
        result = self.run_helper(args=('photo', 'fixture-chat', 'message-id', str(picture)),
                                 first={'stdout': 'ok\n' + base64.b64encode(jpeg).decode() + '\n'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, b'ok\n')
        self.assertEqual(picture.read_bytes(), jpeg)

    def test_sendphoto_is_never_replayed_after_transport_failure(self):
        picture = self.root / 'upload.jpg'
        picture.write_bytes(b'fixture picture')
        result = self.run_helper(args=('sendphoto', 'fixture-chat', str(picture)),
                                 first={'code': 255, 'stderr': 'Connection refused\n'},
                                 second={'stdout': 'ok\n'})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls.read_text().splitlines(), ['192.0.2.145'])

    def test_failed_diagnostic_creation_does_not_leak_paths_or_start_ssh(self):
        self.env['TMPDIR'] = str(self.root / 'private-missing-folder')
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'error: Could not prepare the PC connection', result.stdout)
        self.assertEqual(result.stderr, b'')
        self.assertFalse(self.calls.exists())

    def test_cancel_removes_private_diagnostics_and_preserves_key(self):
        self.env['FAKE_FLOW'] = json.dumps({'192.0.2.145': {'hold': True}})
        proc = subprocess.Popen(['dash', str(HELPER), 'status'], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            deadline = time.monotonic() + 3
            while not self.calls.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertTrue(self.calls.exists(), 'fake SSH did not start')
            diagnostics = list(self.root.glob('nzeus-wa-ssh.*'))
            self.assertEqual(len(diagnostics), 1)
            self.assertEqual(diagnostics[0].stat().st_mode & 0o777, 0o600)
            os.killpg(proc.pid, signal.SIGTERM)
            proc.communicate(timeout=5)
            self.assertEqual(list(self.root.glob('nzeus-wa-ssh.*')), [])
            self.assertEqual(self.key.read_text(), 'Not a real private key\n')
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
