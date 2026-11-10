"""Serves docs/play on http://localhost:8000/ for testing the play page (opening index.html as a file does not work: the page needs a web server).
No caching, so every edit shows at once; correct types for .wasm / .js / .gz.   usage: python scripts/serve_play.py [port] [folder]"""
import functools
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm", ".js": "text/javascript", ".gz": "application/gzip"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
folder = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "play")
print("serving %s on http://localhost:%d/" % (folder, port))
http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(Handler, directory=folder)).serve_forever()
