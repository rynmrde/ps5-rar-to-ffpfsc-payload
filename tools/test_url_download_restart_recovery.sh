#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
bin=${WFM_SERVER_BIN:-./web-file-mgr-linux}
root=/tmp/mkpfs-download-restart-recovery
server_port=${WFM_PORT:-18096}
fixture_port=${URL_FIXTURE_PORT:-18097}
token=mkpfs-download-restart-token-012345
pid=
fixture_pid=

cleanup() {
  kill "${pid:-}" "${fixture_pid:-}" 2>/dev/null || true
  wait "${pid:-}" "${fixture_pid:-}" 2>/dev/null || true
  rm -rf "$root"
}
trap cleanup EXIT INT TERM
rm -rf "$root"
mkdir -p "$root/destination" "$root/journals"
python3 tests/url_download_fixture_server.py --port "$fixture_port" >"$root/fixture.log" 2>&1 &
fixture_pid=$!
for _ in $(seq 1 100); do
  curl -fsS "http://127.0.0.1:$fixture_port/payload.bin" >/dev/null 2>&1 && break
  sleep 0.05
done
curl -fsS "http://127.0.0.1:$fixture_port/payload.bin" >/dev/null

start() {
  WFM_PORT="$server_port" WFM_ACCESS_TOKEN="$token" WFM_DOWNLOAD_DIR="$root/journals" \
    stdbuf -oL -eL "$bin" >"$root/server.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 100); do
    if curl -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$server_port/api/tasks" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  cat "$root/server.log" >&2 || true
  return 1
}

task_state() {
  curl -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$server_port/api/tasks" |
    sed -n 's/.*"state":"\([^"]*\)".*/\1/p' | head -n 1
}

start
response=$(curl -fsS -X POST -H "X-WFM-Token: $token" \
  "http://127.0.0.1:$server_port/api/url-download?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fslow.bin&destination=$root%2Fdestination&name=recover.bin")
printf '%s' "$response" | grep -q '"ok":true'

# Wait until the task has emitted a durable, atomic journal and a private part.
for _ in $(seq 1 100); do
  if find "$root/journals" -type f -name 'mkpfs-download-*.resume' -size +100c | grep -q . &&
     find "$root/destination" -type f -name '.mkpfs-download-*.part' -size +0c | grep -q .; then
    break
  fi
  sleep 0.05
done
find "$root/journals" -type f -name 'mkpfs-download-*.resume' -size +100c | grep -q .
find "$root/destination" -type f -name '.mkpfs-download-*.part' -size +0c | grep -q .
kill -KILL "$pid"
wait "$pid" 2>/dev/null || true
pid=

# A new payload process must restore the task and request only its missing range.
start
state=
for _ in $(seq 1 600); do
  state=$(task_state || true)
  case "$state" in done|failed|canceled) break;; esac
  sleep 0.05
done
[ "$state" = done ]
python3 - "$root/destination/recover.bin" <<'PY'
from pathlib import Path
import sys
assert Path(sys.argv[1]).read_bytes() == bytes(range(256)) * 65536
PY
! find "$root/journals" -type f -name 'mkpfs-download-*.resume' -print | grep -q .
! find "$root/destination" -type f -name '.mkpfs-download-*.part' -print | grep -q .
printf '%s\n' 'download restart recovery test passed'
