#!/usr/bin/env python3
"""Local-only malformed-request regression harness for the host binary."""
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit(f"usage: {sys.argv[0]} SERVER_BINARY LOG_FILE")

server = Path(sys.argv[1]).resolve()
log_path = Path(sys.argv[2]).resolve()
port = int(os.environ.get("WFM_PORT", "18080"))
root = Path("/tmp/mkpfs-http-audit")
(root / "source" / "sce_sys").mkdir(parents=True, exist_ok=True)
(root / "out").mkdir(parents=True, exist_ok=True)
(root / "source" / "sce_sys" / "param.json").write_text('{"titleId":"PTEST0001"}\n')
(root / "source" / "readme.txt").write_text("payload\n")

requests = [
    b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
    b"GET /no-such-route HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
    b"GET /api/list?path=%2Ftmp%2F..%2Fetc HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
    b"POST /api/convert?source=%2Ftmp%2F..%2Fetc&destination=%2Ftmp%2Fmkpfs-http-audit%2Fout&name=BAD.ffpfsc HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    b"POST /api/convert?source=%ZZ&destination=%2Ftmp%2Fmkpfs-http-audit%2Fout&name=%2e%2e%2fBAD.ffpfsc HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Long: " + (b"A" * 65536) + b"\r\nConnection: close\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost\r\n\r\n",
    b"BROKEN\r\n\r\n",
    b"POST /api/list HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4294967295\r\nConnection: close\r\n\r\n",
]
token = "mkpfs-http-audit-token-012345678"
token_header = b"X-WFM-Token: " + token.encode() + b"\r\n"
requests = [
    request if not request.startswith((b"GET /api/", b"POST /api/")) else
    request.replace(b"\r\n\r\n", b"\r\n" + token_header + b"\r\n", 1)
    for request in requests
]

def send_raw(payload: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(payload)
        sock.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            try:
                chunk = sock.recv(16384)
            except socket.timeout:
                break
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)

env = os.environ.copy()
env["WFM_ACCESS_TOKEN"] = token
env["WFM_PORT"] = str(port)
with log_path.open("wb") as log:
    proc = subprocess.Popen([str(server)], stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        deadline = time.monotonic() + 10
        while True:
            if proc.poll() is not None:
                raise RuntimeError(f"server terminated before readiness: {proc.returncode}")
            try:
                response = send_raw(requests[0])
                if b"200" not in response.split(b"\r\n", 1)[0]:
                    raise RuntimeError(f"unexpected readiness response: {response[:120]!r}")
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        response = send_raw(b"POST /api/tasks HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        if b"403" not in response.split(b"\r\n", 1)[0]:
            raise RuntimeError(f"unauthenticated API request was not denied: {response[:120]!r}")
        for index, payload in enumerate(requests, 1):
            response = send_raw(payload)
            if proc.poll() is not None:
                raise RuntimeError(f"server died after malformed request {index}: {proc.returncode}")
            # libmicrohttpd is permitted to close a malformed connection without
            # serializing a response.  Availability of the server after the
            # request, rather than a response to an invalid wire format, is the
            # safety requirement.
            if index == 1 and b"200" not in response.split(b"\r\n", 1)[0]:
                raise RuntimeError(f"valid root request failed: {response[:120]!r}")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

log_data = log_path.read_text(errors="replace")
for marker in ("ERROR: AddressSanitizer", "runtime error:", "UndefinedBehaviorSanitizer"):
    if marker in log_data:
        raise RuntimeError(f"sanitizer reported {marker}")
print(f"HTTP_ROBUSTNESS_PASS server_exit={proc.returncode} requests={len(requests)}")
