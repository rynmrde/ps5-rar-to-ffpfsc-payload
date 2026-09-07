#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
root=/tmp/mkpfs-url-download-http
server_port=${WFM_PORT:-18084}
fixture_port=${URL_FIXTURE_PORT:-18085}
token=mkpfs-url-download-token-012345678
server_bin=${WFM_SERVER_BIN:-./web-file-mgr-linux}
rm -rf "$root"
mkdir -p "$root/destination"

python3 tests/url_download_fixture_server.py --port "$fixture_port" >"$root/fixture.log" 2>&1 &
fixture_pid=$!
WFM_PORT="$server_port" WFM_ACCESS_TOKEN="$token" "$server_bin" >"$root/server.log" 2>&1 &
server_pid=$!
cleanup() {
  kill "$server_pid" "$fixture_pid" 2>/dev/null || true
  wait "$server_pid" "$fixture_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 80); do
  if curl --compressed -fsS "http://127.0.0.1:$server_port/" >/dev/null 2>&1 &&
     curl -fsS "http://127.0.0.1:$fixture_port/payload.bin" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

api_post() {
  curl --compressed -fsS -H "X-WFM-Token: $token" -X POST "$1"
}

task_id_from() {
  printf '%s' "$1" | sed -n 's/.*"task_id":\([0-9][0-9]*\).*/\1/p'
}

task_state() {
  id=$1
  response=$(curl --compressed -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$server_port/api/tasks")
  printf '%s' "$response" | sed -n "s/.*\"id\":$id[^}]*\"state\":\"\([^\"]*\)\".*/\1/p"
}

wait_for_state() {
  id=$1
  expected=$2
  state=''
  for _ in $(seq 1 160); do
    state=$(task_state "$id")
    [ "$state" = "$expected" ] && return 0
    case "$state" in failed|canceled|done) break;; esac
    sleep 0.1
  done
  echo "task $id state was ${state:-missing}, expected $expected" >&2
  return 1
}

base="http://127.0.0.1:$server_port/api/url-download"
destination=$(printf '%s' "$root/destination" | sed 's|/|%2F|g')

# Successful direct URL download with known length and atomic publication.
response=$(api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fpayload.bin&destination=$destination&name=payload.bin")
id=$(task_id_from "$response")
[ -n "$id" ]
wait_for_state "$id" done
python3 - "$root/destination/payload.bin" <<'PY'
from pathlib import Path
import sys
expected = bytes(range(256)) * 8192
assert Path(sys.argv[1]).read_bytes() == expected
PY
[ ! -e "$root/destination/.mkpfs-download-$id-XXXXXX" ]

# A response without Content-Length still completes and publishes safely.
response=$(api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fno-length.bin&destination=$destination&name=stream.bin")
id=$(task_id_from "$response")
[ -n "$id" ]
wait_for_state "$id" done
cmp "$root/destination/payload.bin" "$root/destination/stream.bin"

# Filename traversal, unsupported schemes, and accidental overwrite are rejected.
if api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fpayload.bin&destination=$destination&name=..%2Fescape.bin" >/dev/null 2>&1; then
  echo 'unsafe URL download filename was accepted' >&2
  exit 1
fi
if api_post "$base?url=ftp%3A%2F%2Fexample.invalid%2Ffile&destination=$destination&name=bad.bin" >/dev/null 2>&1; then
  echo 'unsupported URL scheme was accepted' >&2
  exit 1
fi
if api_post "$base?url=http%3A%2F%2Fuser%40example.invalid%2Ffile&destination=$destination&name=credentials.bin" >/dev/null 2>&1; then
  echo 'URL credentials were accepted' >&2
  exit 1
fi
if api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fpayload.bin&destination=$destination&name=payload.bin" >/dev/null 2>&1; then
  echo 'existing URL download destination was accepted' >&2
  exit 1
fi

# HTTP failure leaves no partial final destination or task-private staging file.
response=$(api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fmissing.bin&destination=$destination&name=missing.bin")
id=$(task_id_from "$response")
[ -n "$id" ]
wait_for_state "$id" failed
[ ! -e "$root/destination/missing.bin" ]
! find "$root/destination" -maxdepth 1 -name '.mkpfs-download-*' -print | grep -q .

# Redirects are deliberately not followed, so a final direct URL is required.
response=$(api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fredirect.bin&destination=$destination&name=redirect.bin")
id=$(task_id_from "$response")
[ -n "$id" ]
wait_for_state "$id" failed
[ ! -e "$root/destination/redirect.bin" ]
! find "$root/destination" -maxdepth 1 -name '.mkpfs-download-*' -print | grep -q .

# Cancellation removes partial state and leaves the destination name unused.
response=$(api_post "$base?url=http%3A%2F%2F127.0.0.1%3A$fixture_port%2Fslow.bin&destination=$destination&name=slow.bin")
id=$(task_id_from "$response")
[ -n "$id" ]
sleep 0.2
api_post "http://127.0.0.1:$server_port/api/cancel?id=$id" >/dev/null
wait_for_state "$id" canceled
[ ! -e "$root/destination/slow.bin" ]
! find "$root/destination" -maxdepth 1 -name '.mkpfs-download-*' -print | grep -q .

echo 'URL download HTTP integration test passed'
