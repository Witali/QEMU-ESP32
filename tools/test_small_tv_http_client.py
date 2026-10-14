"""Check the real loopback HTTP client's JSON bytes, defaults and host boundary."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import unittest
from small_tv_http_test import http


class HttpClientTests(unittest.TestCase):
    """Receive actual socket requests without replacing the client's serializer."""
    def test_utf8_and_default_json_bytes(self):
        """Keep the previous escaped default and opt into JSON.stringify-style UTF-8."""
        bodies=[]
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):
                """Suppress fixture access logs; they contain only synthetic requests."""
                pass
            def do_POST(self):
                """Capture actual wire bytes and return a bounded success response."""
                bodies.append(self.rfile.read(int(self.headers['Content-Length'])))
                self.send_response(200)
                self.send_header('Content-Length','2')
                self.end_headers()
                self.wfile.write(b'OK')
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True)
        thread.start()
        try:
            url=f'http://127.0.0.1:{server.server_address[1]}'
            payload={'city':'Тест,CN'}
            self.assertEqual(http(url,'/fixture',payload),(200,b'OK'))
            self.assertEqual(http(url,'/fixture',payload,ensure_ascii=False),(200,b'OK'))
            self.assertIn(b'\\u0422',bodies[0])
            self.assertNotIn(b'\\u',bodies[1])
            self.assertIn('Тест'.encode('utf-8'),bodies[1])
            self.assertEqual(json.loads(bodies[0]),payload)
            self.assertEqual(json.loads(bodies[1]),payload)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_non_loopback_rejected(self):
        """Reject an external endpoint before making a connection."""
        with self.assertRaises(ValueError):
            http('http://example.com','/',{'city':'Synthetic,CN'},ensure_ascii=False)


if __name__=='__main__':unittest.main(verbosity=2)
