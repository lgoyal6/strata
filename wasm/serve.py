#!/usr/bin/env python3
"""Local demo server with cross-origin isolation headers (wasm pthreads)."""
import http.server, functools, os

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

docs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
http.server.ThreadingHTTPServer(
    ("127.0.0.1", 8433),
    functools.partial(Handler, directory=docs),
).serve_forever()
