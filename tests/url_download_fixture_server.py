#!/usr/bin/env python3
"""Local direct-URL fixture server for the native downloader regression test."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import argparse
import time

PAYLOAD = bytes(range(256)) * 8192  # 2 MiB deterministic payload.
SLOW_PAYLOAD = bytes(range(256)) * 65536  # 16 MiB for cancellation coverage.


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        return

    def send_payload(self, payload, slow=False, ignore_range=False):
        start = 0
        range_header = self.headers.get("Range", "")
        if range_header and not ignore_range:
            if not range_header.startswith("bytes=") or not range_header.endswith("-"):
                self.send_error(416, "invalid range")
                return
            try:
                start = int(range_header[6:-1])
            except ValueError:
                self.send_error(416, "invalid range")
                return
            if start < 0 or start >= len(payload):
                self.send_error(416, "range unavailable")
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(payload) - 1}/{len(payload)}")
            payload = payload[start:]
        else:
            self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        for offset in range(0, len(payload), 16384):
            self.wfile.write(payload[offset:offset + 16384])
            self.wfile.flush()
            if slow:
                time.sleep(0.02)

    def do_GET(self):
        if self.path == "/payload.bin":
            self.send_payload(PAYLOAD)
        elif self.path == "/slow.bin":
            self.send_payload(SLOW_PAYLOAD, slow=True)
        elif self.path == "/ignore-range.bin":
            self.send_payload(SLOW_PAYLOAD, slow=True, ignore_range=True)
        elif self.path == "/no-length.bin":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(PAYLOAD)
        elif self.path == "/redirect.bin":
            self.send_response(302)
            self.send_header("Location", "/payload.bin")
            self.send_header("Connection", "close")
            self.end_headers()
        else:
            self.send_error(404, "fixture not found")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
