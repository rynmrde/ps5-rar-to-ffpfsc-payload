#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

root=/tmp/mkpfs-http-archive
port=${WFM_PORT:-18889}
token=mkpfs-http-archive-token-012345678
urlencode_path() { printf '%s' "$1" | sed 's|/|%2F|g'; }
wait_task() {
  id=$1
  state=''
  for i in $(seq 1 200); do
    tasks=$(curl --compressed -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$port/api/tasks")
    state=$(printf '%s' "$tasks" | sed -n "s/.*\"id\":$id[^}]*\"state\":\"\([^\"]*\)\".*/\1/p")
    case "$state" in done|failed|canceled) break;; esac
    sleep 0.1
  done
  [ "$state" = done ]
}
queue_extract() {
  archive=$1
  name=$2
  curl --compressed -fsS -H "X-WFM-Token: $token" -X POST \
    "http://127.0.0.1:$port/api/extract?source=$(urlencode_path "$archive")&destination=$(urlencode_path "$root/out")&name=$name&workers=1"
}

rm -rf "$root"
mkdir -p "$root/input/nested" "$root/out"
printf 'native archive API\n' > "$root/input/nested/result.txt"
dd if=/dev/urandom of="$root/input/payload.bin" bs=1024 count=64 status=none
(
  cd "$root/input"
  rar a -idq "$root/sample.rar" nested/result.txt payload.bin
  7z a -bd -y "$root/sample.7z" nested/result.txt payload.bin >/dev/null
)
WFM_PORT="$port" WFM_ACCESS_TOKEN="$token" ./web-file-mgr-linux > "$root/server.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -rf "$root"' EXIT INT TERM
for i in $(seq 1 80); do
  if curl --compressed -fsS "http://127.0.0.1:$port/" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
curl --compressed -fsS "http://127.0.0.1:$port/" >/dev/null

for archive in "$root/sample.rar" "$root/sample.7z"; do
  base=$(basename "$archive")
  response=$(queue_extract "$archive" "$base-out")
  id=$(printf '%s' "$response" | sed -n 's/.*"task_id":\([0-9][0-9]*\).*/\1/p')
  [ -n "$id" ]
  wait_task "$id"
  cmp "$root/input/nested/result.txt" "$root/out/$base-out/nested/result.txt"
done

if queue_extract "$root/sample.rar" 'sample.rar-out' >/dev/null 2>&1; then
  echo 'existing archive output unexpectedly accepted' >&2
  exit 1
fi
if curl --compressed -fsS -H "X-WFM-Token: $token" -X POST \
  "http://127.0.0.1:$port/api/extract?source=%2Ftmp%2F..%2Fetc%2Fpasswd&destination=$(urlencode_path "$root/out")&name=bad" >/dev/null 2>&1; then
  echo 'traversal archive source unexpectedly accepted' >&2
  exit 1
fi

# A stored 512 MiB member keeps the worker active long enough for the API
# cancellation path to be observed without consuming unbounded memory.
dd if=/dev/zero of="$root/input/cancel.bin" bs=1048576 count=512 status=none
(cd "$root/input" && 7z a -bd -y -m0=Copy "$root/cancel.7z" cancel.bin >/dev/null)
response=$(queue_extract "$root/cancel.7z" 'cancel-out')
id=$(printf '%s' "$response" | sed -n 's/.*"task_id":\([0-9][0-9]*\).*/\1/p')
[ -n "$id" ]
curl --compressed -fsS -H "X-WFM-Token: $token" -X POST \
  "http://127.0.0.1:$port/api/cancel?id=$id" >/dev/null
state=''
for i in $(seq 1 200); do
  tasks=$(curl --compressed -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$port/api/tasks")
  state=$(printf '%s' "$tasks" | sed -n "s/.*\"id\":$id[^}]*\"state\":\"\([^\"]*\)\".*/\1/p")
  case "$state" in done|failed|canceled) break;; esac
  sleep 0.1
done
[ "$state" = canceled ]
[ ! -e "$root/out/cancel-out" ]
[ ! -e "$root/out/.mkpfs-extract-$id.tmp" ]
printf 'archive HTTP extraction, task status, collision, traversal, cancellation, and cleanup tests passed\n'
