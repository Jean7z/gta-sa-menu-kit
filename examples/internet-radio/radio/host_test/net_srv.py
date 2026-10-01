#!/usr/bin/env python3
"""Test server for the Slice B1 HTTP client.
   /test.mp3  -> the whole file
   /redir     -> 302 to /test.mp3 (exercises redirect following)
   /dribble   -> same file in 128-byte flushes with 2 ms pauses (exercises the
                 decoder carry path with minimal network-sized packets)
"""
import os
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
FILE = os.path.join(HERE, "test.mp3")
PORT = 8098


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def _body(self, data):
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        return data

    def do_GET(self):
        if self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:%d/test.mp3" % PORT)
            self.end_headers()
            return
        if self.path not in ("/test.mp3", "/dribble"):
            self.send_error(404)
            return

        data = self._body(open(FILE, "rb").read())
        if self.path == "/dribble":
            for i in range(0, len(data), 128):
                self.wfile.write(data[i:i + 128])
                self.wfile.flush()
                time.sleep(0.002)
        else:
            self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
