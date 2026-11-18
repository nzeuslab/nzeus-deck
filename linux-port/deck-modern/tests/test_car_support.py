"""Vehicle data trust tests. Uses a disposable signing key; never talks to cars."""
import copy
import hashlib
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time
import io
import http.server
import ssl
import threading
import fcntl
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / 'nzeus-car-support'


class SupportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = tempfile.TemporaryDirectory()
        cls.key = Path(cls.keys.name) / 'key.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
                        'rsa_keygen_bits:2048', '-out', str(cls.key)], check=True, capture_output=True)
        cls.public = subprocess.check_output(['openssl', 'pkey', '-in', str(cls.key), '-pubout'])

    @classmethod
    def tearDownClass(cls):
        cls.keys.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        if PATH.exists():
            loader = importlib.machinery.SourceFileLoader('car_support', str(PATH))
            spec = importlib.util.spec_from_loader(loader.name, loader)
            self.m = importlib.util.module_from_spec(spec)
            loader.exec_module(self.m)
        else:
            self.m = None
        self.pack = dict(schema=1, id='silverado-2016-guide', version=1, kind='owner-guide',
                         make='Chevrolet', model='Silverado', year=2016, market='US',
                         title='Owner-menu guide', source='https://www.chevrolet.com/manual.pdf',
                         features=[dict(id='unlock', title='Door unlocking',
                                        description='Choose which doors unlock.',
                                        instructions='Use the vehicle menu if equipped.')])
        self.data = json.dumps(self.pack).encode()
        self.catalog = dict(schema=1, generation=1, expires=int(time.time()) + 86400,
                            packs=[dict(id=self.pack['id'], version=1,
                                        sha256=hashlib.sha256(self.data).hexdigest(),
                                        bytes=len(self.data), file='silverado-2016-guide.json')])

    def tearDown(self):
        self.temp.cleanup()

    def require(self):
        self.assertIsNotNone(self.m, 'Vehicle support manager is not implemented')

    def signed(self, catalog=None):
        raw = json.dumps(catalog or self.catalog).encode()
        sig = subprocess.check_output(['openssl', 'dgst', '-sha256', '-sign', str(self.key)], input=raw)
        return raw, sig

    def install(self, raw=None, sig=None, data=None):
        raw0, sig0 = self.signed()
        blobs = {'catalog.json': raw or raw0, 'catalog.sig': sig or sig0,
                 'silverado-2016-guide.json': self.data if data is None else data}
        return self.m.install(self.root, blobs.__getitem__, self.public)

    def test_signed_pack_survives_offline_and_is_not_a_toggle(self):
        self.require()
        self.install()
        packs = self.m.installed(self.root, self.public)
        self.assertEqual(packs[0]['features'][0]['id'], 'unlock')
        text = self.m.describe(self.root, dict(make='Chevrolet', model='Silverado', year=2016,
                                              market='US'), self.public)
        self.assertIn('Owner-menu guide', text)
        self.assertIn('not read', text)
        self.assertIn('0 verified app-controlled changes', text)

    def test_bad_signature_preserves_prior_generation(self):
        self.require()
        self.install()
        before = (self.root / 'current.json').read_bytes()
        raw, sig = self.signed()
        with self.assertRaises(ValueError):
            self.install(raw=raw + b' ', sig=sig)
        self.assertEqual((self.root / 'current.json').read_bytes(), before)

    def test_hash_and_size_fail_closed(self):
        self.require()
        for data in [b'{}', self.data + b' ']:
            with self.assertRaises(ValueError):
                self.install(data=data)
        self.assertFalse((self.root / 'current.json').exists())

    def test_signed_commands_and_coding_are_rejected(self):
        self.require()
        for mutation in [('kind', 'coding'), ('command', '2EF19000')]:
            pack = copy.deepcopy(self.pack)
            pack[mutation[0]] = mutation[1]
            with self.assertRaises(ValueError):
                self.m.validate_pack(pack)
        pack = copy.deepcopy(self.pack)
        pack['features'][0]['write'] = '2E0000'
        with self.assertRaises(ValueError):
            self.m.validate_pack(pack)

    def test_expired_catalog_and_traversal_rejected(self):
        self.require()
        for change in ['expired', 'traversal']:
            catalog = copy.deepcopy(self.catalog)
            if change == 'expired':
                catalog['expires'] = 1
            else:
                catalog['packs'][0]['file'] = '../escape'
            raw, sig = self.signed(catalog)
            with self.assertRaises(ValueError):
                self.install(raw=raw, sig=sig)

    def test_downgrade_and_same_generation_equivocation_rejected(self):
        self.require()
        high = copy.deepcopy(self.catalog)
        high['generation'] = 2
        raw, sig = self.signed(high)
        self.install(raw=raw, sig=sig)
        with self.assertRaises(ValueError):
            self.install()
        high['expires'] += 1
        raw, sig = self.signed(high)
        with self.assertRaises(ValueError):
            self.install(raw=raw, sig=sig)

    def test_incomplete_refresh_preserves_offline_pack(self):
        self.require()
        self.install()
        raw, sig = self.signed()
        def interrupted(name):
            if name == 'catalog.json': return raw
            if name == 'catalog.sig': return sig
            raise OSError('network interrupted')
        with self.assertRaises(OSError):
            self.m.install(self.root, interrupted, self.public)
        self.assertEqual(len(self.m.installed(self.root, self.public)), 1)

    def test_tampered_cache_rejected_on_use(self):
        self.require()
        self.install()
        for path in self.root.glob('generations/*/silverado-2016-guide.json'):
            path.write_bytes(b'{}')
        with self.assertRaises(ValueError):
            self.m.installed(self.root, self.public)

    def test_wrong_model_year_market_never_matches(self):
        self.require()
        self.install()
        for field, value in [('model', 'Ranger'), ('year', 2015), ('market', 'GCC')]:
            selection = dict(make='Chevrolet', model='Silverado', year=2016, market='US')
            selection[field] = value
            self.assertNotIn('Door unlocking', self.m.describe(self.root, selection, self.public))

    def test_https_only_bounded_no_credentials_no_cross_origin_redirect(self):
        self.require()
        for url in ['http://example.com', 'https://u:p@example.com', 'https://example.com/?token=x',
                    'file:///tmp/catalog']:
            with self.assertRaises(ValueError):
                self.m.https_base(url)
        self.assertEqual(self.m.https_base('https://example.com/car/'), 'https://example.com/car/')
        with self.assertRaises(ValueError):
            self.m.SameOrigin('https://example.com/').redirect_request(None, None, 302, '', {},
                                                                      'http://example.com/no')

    def test_duplicate_json_keys_and_bad_text_rejected(self):
        self.require()
        with self.assertRaises(ValueError):
            self.m.decode(b'{"schema":1,"schema":2}')
        pack = copy.deepcopy(self.pack)
        pack['title'] = '\x1b[2J'
        with self.assertRaises(ValueError):
            self.m.validate_pack(pack)

    def test_unknown_selection_and_no_server_are_honest(self):
        self.require()
        self.assertIn('Choose', self.m.describe(self.root, {}, self.public))
        self.assertIn('not configured', self.m.refresh(self.root, None, self.public))

    def test_refresh_downloads_signed_catalog_and_pack_without_vin(self):
        self.require()
        raw, sig = self.signed()
        blobs = {'catalog.json': raw, 'catalog.sig': sig, 'silverado-2016-guide.json': self.data}
        requests = []
        class Opener:
            def open(inner, request, timeout):
                requests.append(request.full_url)
                return io.BytesIO(blobs[request.full_url.rsplit('/', 1)[1]])
        with patch.object(self.m.urllib.request, 'build_opener', return_value=Opener()):
            self.assertIn('Verified 1', self.m.refresh(self.root, 'https://packs.example/car', self.public))
        self.assertEqual(requests, ['https://packs.example/car/catalog.json',
                                    'https://packs.example/car/catalog.sig',
                                    'https://packs.example/car/silverado-2016-guide.json'])

    def test_atomic_commit_failure_keeps_old_pointer_and_pack(self):
        self.require()
        self.install()
        before = (self.root / 'current.json').read_bytes()
        new = copy.deepcopy(self.catalog)
        new['generation'] = 2
        raw, sig = self.signed(new)
        with patch.object(self.m, 'atomic_json', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                self.install(raw=raw, sig=sig)
        self.assertEqual((self.root / 'current.json').read_bytes(), before)
        self.assertEqual(len(self.m.installed(self.root, self.public)), 1)

    def test_package_downgrade_and_revocation(self):
        self.require()
        original_data = self.data
        self.pack['version'] = 2
        self.data = json.dumps(self.pack).encode()
        self.catalog['packs'][0].update(version=2, bytes=len(self.data), sha256=hashlib.sha256(self.data).hexdigest())
        self.install()
        revoked = copy.deepcopy(self.catalog)
        revoked.update(generation=2, packs=[])
        raw, sig = self.signed(revoked)
        self.install(raw=raw, sig=sig)
        self.assertEqual(self.m.installed(self.root, self.public), [])
        old_pack = copy.deepcopy(self.catalog)
        old_pack.update(generation=3)
        old_pack['packs'][0].update(version=1, bytes=len(original_data), sha256=hashlib.sha256(original_data).hexdigest())
        raw, sig = self.signed(old_pack)
        with self.assertRaises(ValueError):
            self.install(raw=raw, sig=sig, data=original_data)

    def test_parallel_install_is_refused(self):
        self.require()
        with (self.root / '.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                self.install()

    def test_idempotent_refresh_retains_previous_generation(self):
        self.require()
        self.install()
        previous = json.loads((self.root / 'current.json').read_text())['digest']
        self.catalog['generation'] = 2
        self.install()
        self.install()
        self.assertEqual(json.loads((self.root / 'current.json').read_text())['previous'], previous)
        self.assertTrue((self.root / 'generations' / previous).is_dir())

    def test_real_https_delivery_and_certificate_validation(self):
        self.require()
        raw, sig = self.signed()
        blobs = {'/catalog.json': raw, '/catalog.sig': sig, '/silverado-2016-guide.json': self.data}
        cert, key = self.root / 'cert.pem', self.root / 'tls-key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1',
                        '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(inner):
                data = blobs.get(inner.path, b'')
                inner.send_response(200 if data else 404)
                inner.end_headers()
                inner.wfile.write(data)
            def log_message(self, *args): pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f'https://127.0.0.1:{server.server_port}'
        try:
            # Production's default CA validation refuses this self-signed test server.
            with self.assertRaises(OSError):
                self.m.refresh(self.root / 'untrusted', url, self.public)
            original = self.m.urllib.request.build_opener
            trusted = ssl.create_default_context(cafile=str(cert))
            def opener(*handlers):
                return original(*handlers, self.m.urllib.request.HTTPSHandler(context=trusted))
            with patch.object(self.m.urllib.request, 'build_opener', side_effect=opener):
                self.assertIn('Verified 1', self.m.refresh(self.root / 'trusted', url, self.public))
            self.assertEqual(len(self.m.installed(self.root / 'trusted', self.public)), 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
