import base64
import http.client
import json
import tempfile
import threading
import unittest
from pathlib import Path
from local_auth.profiles import ProfileStore
try:
    from local_auth.server import make_server
except ImportError:
    make_server = None


class ServerTests(unittest.TestCase):
    def test_invalid_requests_do_not_issue_sessions(self):
        self.assertIsNotNone(make_server)
        with tempfile.TemporaryDirectory() as directory:
            player = ProfileStore(Path(directory) / 'profiles.db').get_or_create('Tester')
            server = make_server(player, port=0)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for method, path, body, expected in [
                    ('POST', '/auth/', '{', 400),
                    ('POST', '/auth/', '[]', 400),
                    ('POST', '/auth/', json.dumps({'auth_task': '29', 'title_id': '1002', 'iv_seed': '1'}), 400),
                    ('POST', '/wrong', '{}', 404),
                    ('GET', '/wrong', None, 404),
                ]:
                    with self.subTest(method=method, path=path, body=body):
                        conn = http.client.HTTPConnection(*server.server_address, timeout=3)
                        try:
                            conn.request(method, path, body)
                            response = conn.getresponse()
                            self.assertEqual(response.status, expected)
                            response.read()
                        finally:
                            conn.close()
                self.assertEqual(len(server.sessions), 0)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(3)

    def test_tls_accepts_qa_style_expect_continue_request(self):
        import ssl
        import warnings
        certificate = Path(__file__).resolve().parents[1] / 'pki' / 'server.pem'
        ca = Path(__file__).resolve().parents[1] / 'pki' / 'ca.pem'
        if not certificate.exists() or not ca.exists():
            self.skipTest('no pki/ (run make_ca.py)')
        with tempfile.TemporaryDirectory() as directory:
            player = ProfileStore(Path(directory) / 'profiles.db').get_or_create('TLSPlayer')
            with warnings.catch_warnings():
                warnings.simplefilter('ignore', DeprecationWarning)
                server = make_server(player, port=0, certfile=str(certificate))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            context = ssl.create_default_context(cafile=str(ca))
            # Existing local certificate predates strict AKI enforcement.
            # Chain and hostname verification remain enabled.
            context.verify_flags &= ~ssl.VERIFY_X509_STRICT
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            conn = http.client.HTTPSConnection(*server.server_address, context=context, timeout=3)
            try:
                conn.request('POST', '/auth/', json.dumps({'auth_task': '10', 'title_id': '1002', 'iv_seed': '42'}),
                             {'Content-Type': 'application/json', 'Expect': '100-continue'})
                response = conn.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(json.loads(response.read())['code'], '700')
            finally:
                conn.close()
                server.shutdown()
                server.server_close()
                thread.join(3)

    def test_auth_over_http_issues_selected_local_name(self):
        self.assertIsNotNone(make_server, 'HTTP auth service is not implemented')
        with tempfile.TemporaryDirectory() as directory:
            player = ProfileStore(Path(directory) / 'profiles.db').get_or_create('MenuTester')
            server = make_server(player, port=0)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            conn = http.client.HTTPConnection(*server.server_address, timeout=3)
            try:
                conn.request('POST', '/auth/', json.dumps({'auth_task': '10', 'title_id': '1002', 'iv_seed': '42'}),
                             {'Content-Type': 'application/json'})
                response = conn.getresponse()
                self.assertEqual(response.status, 200)
                result = json.loads(response.read())
                self.assertEqual(result['auth_task'], '11')
                ticket = base64.b64decode(result['client_ticket'])
                self.assertEqual(ticket[33:97].rstrip(b'\0'), b'MenuTester')
                self.assertEqual(len(server.sessions), 1)
                session = next(iter(server.sessions.values()))
                self.assertEqual(session['iv_seed'], 42)
                self.assertEqual(session['title_id'], 1002)
            finally:
                conn.close()
                server.shutdown()
                server.server_close()
                thread.join(3)


if __name__ == '__main__':
    unittest.main()
