#!/usr/bin/env python3
"""Serve the HLS output directory to the Chromecast.

The Cast default media receiver fetches HLS with XHR, so every response needs CORS headers.
Playlists must not be cached, or the receiver keeps playing the first segments.
"""
import http.server
import sys
from functools import partial


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".m3u8": "application/vnd.apple.mpegurl",
        ".ts": "video/mp2t",
    }

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        if self.path.endswith(".m3u8"):
            self.send_header("Cache-Control", "no-cache, no-store")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


def main():
    directory, port = sys.argv[1], int(sys.argv[2])
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), partial(Handler, directory=directory))
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
