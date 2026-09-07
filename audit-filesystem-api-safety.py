#!/usr/bin/env python3
"""Local disposable-fixture API regressions for destructive path handling."""
import os
import shutil
import signal
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

repo = Path(__file__).resolve().parent
binary = repo / "web-file-mgr-linux"
token = "mkpfs-api-safety-token-012345678"
port = int(os.environ.get("WFM_PORT", "18081"))
root = Path(tempfile.mkdtemp(prefix="mkpfs-api-safety."))


def call(path, *, data=None, method="POST", auth=True):
    request = urllib.request.Request(f"http://127.0.0.1:{port}" + path,
                                     data=data, method=method)
    if auth:
        request.add_header("X-WFM-Token", token)
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()

try:
    source = root / "source"
    output = root / "output"
    source.mkdir()
    output.mkdir()
    (source / "file.txt").write_text("fixture")
    # A canonical-alias test uses a symlinked spelling of the source directory.
    alias = root / "alias"
    os.symlink(source, alias)
    # A basename-collision fixture must be rejected before a worker starts.
    (root / "left").mkdir()
    (root / "right").mkdir()
    (root / "left" / "same.bin").write_text("left")
    (root / "right" / "same.bin").write_text("right")

    env = os.environ.copy()
    env["WFM_ACCESS_TOKEN"] = token
    env["WFM_PORT"] = str(port)
    server = subprocess.Popen([str(binary)], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, env=env)
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                status, _ = call("/", method="GET", auth=False)
                if status == 200:
                    break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise RuntimeError("server did not become ready")
            time.sleep(0.1)

        status, _ = call("/api/tasks", auth=False)
        if status != 403:
            raise RuntimeError(f"unauthenticated API status {status}, wanted 403")
        status, _ = call("/api/delete", data=urllib.parse.urlencode({"paths": "/tmp/.."}).encode())
        if status != 400:
            raise RuntimeError(f"root alias deletion status {status}, wanted 400")
        status, _ = call("/api/exit", method="GET")
        if status != 405:
            raise RuntimeError(f"GET exit status {status}, wanted 405")
        status, _ = call("/api/copy", data=urllib.parse.urlencode({
            "paths": str(source), "dst": str(alias / "inside"), "overwrite": "0"
        }).encode())
        if status != 409 or (source / "inside").exists():
            raise RuntimeError(f"source-descendant alias copy status {status}")
        status, _ = call("/api/copy", data=urllib.parse.urlencode({
            "paths": str(root / "left" / "same.bin") + "\n" + str(root / "right" / "same.bin"),
            "dst": str(output), "overwrite": "0"
        }).encode())
        if status != 409 or list(output.iterdir()):
            raise RuntimeError(f"basename collision copy status {status}")
        status, body = call("/api/list?" + urllib.parse.urlencode({"path": str(root / "output")}), method="POST")
        if status != 200 or b'"ok":true' not in body:
            raise RuntimeError("authenticated file browse failed")
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
            server.wait(timeout=5)
    print("FILESYSTEM_API_SAFETY_PASS")
finally:
    shutil.rmtree(root, ignore_errors=True)
