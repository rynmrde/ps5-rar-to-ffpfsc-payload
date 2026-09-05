#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=/tmp/mkpfs-http-smoke
rm -rf "$root"
mkdir -p "$root/source/sce_sys" "$root/out"
printf '{"titleId":"PTEST0001"}\n' > "$root/source/sce_sys/param.json"
printf 'payload\n' > "$root/source/readme.txt"
./web-file-mgr-linux > "$root/server.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT
for i in $(seq 1 40); do
  if curl --compressed -fsS http://127.0.0.1:8888/ >/tmp/mkpfs-http-index.html 2>/dev/null; then break; fi
  sleep 0.1
done
grep -q 'Web File Manager' /tmp/mkpfs-http-index.html
queued=$(curl --compressed -fsS -X POST "http://127.0.0.1:8888/api/convert?source=$(printf '%s' "$root/source" | sed 's|/|%2F|g')&destination=$(printf '%s' "$root/out" | sed 's|/|%2F|g')&name=HTTP.ffpfsc&profile=7&workers=4")
printf '%s\n' "$queued"
task_id=$(printf '%s' "$queued" | sed -n 's/.*"task_id":\([0-9][0-9]*\).*/\1/p')
[ -n "$task_id" ]
state=''
for i in $(seq 1 100); do
  tasks=$(curl --compressed -fsS http://127.0.0.1:8888/api/tasks)
  state=$(printf '%s' "$tasks" | sed -n "s/.*\"id\":$task_id[^}]*\"state\":\"\([^\"]*\)\".*/\1/p")
  case "$state" in done|failed|canceled) break;; esac
  sleep 0.1
done
printf 'task_state=%s\n' "$state"
[ "$state" = done ]
[ -f "$root/out/HTTP.ffpfsc" ]
if curl --compressed -fsS -X POST "http://127.0.0.1:8888/api/convert?source=%2Ftmp%2F..%2Fetc&destination=$(printf '%s' "$root/out" | sed 's|/|%2F|g')&name=BAD.ffpfsc" >/tmp/mkpfs-http-invalid.out 2>/dev/null; then
  echo 'invalid source unexpectedly accepted' >&2; exit 1
fi
printf 'http smoke passed\n'
