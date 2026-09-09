#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=/tmp/mkpfs-http-smoke
token=mkpfs-http-smoke-token-012345678
port=${WFM_PORT:-18888}
rm -rf "$root"
mkdir -p "$root/source/sce_sys" "$root/out"
printf '{"titleId":"PTEST0001"}\n' > "$root/source/sce_sys/param.json"
printf 'payload\n' > "$root/source/readme.txt"
WFM_PORT="$port" WFM_ACCESS_TOKEN="$token" ./web-file-mgr-linux > "$root/server.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT
for i in $(seq 1 40); do
  if curl --compressed -fsS "http://127.0.0.1:$port/" >/tmp/mkpfs-http-index.html 2>/dev/null; then break; fi
  sleep 0.1
done
grep -q 'RAR to FFPFSC PS5 Payload' /tmp/mkpfs-http-index.html
listing=$(curl --compressed -fsS -H "X-WFM-Token: $token" \
  "http://127.0.0.1:$port/api/list?path=$(printf '%s' "$root/source" | sed 's|/|%2F|g')")
printf '%s\n' "$listing" | grep -F '"ok":true'
printf '%s\n' "$listing" | grep -F '"name":"readme.txt"'
queued=$(curl --compressed -fsS -H "X-WFM-Token: $token" -X POST "http://127.0.0.1:$port/api/convert?source=$(printf '%s' "$root/source" | sed 's|/|%2F|g')&destination=$(printf '%s' "$root/out" | sed 's|/|%2F|g')&name=HTTP.ffpfsc&profile=7&workers=4")
printf '%s\n' "$queued"
task_id=$(printf '%s' "$queued" | sed -n 's/.*"task_id":\([0-9][0-9]*\).*/\1/p')
[ -n "$task_id" ]
state=''
for i in $(seq 1 100); do
  tasks=$(curl --compressed -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$port/api/tasks")
  state=$(printf '%s' "$tasks" | sed -n "s/.*\"id\":$task_id[^}]*\"state\":\"\([^\"]*\)\".*/\1/p")
  case "$state" in done|failed|canceled) break;; esac
  sleep 0.1
done
printf 'task_state=%s\n' "$state"
[ "$state" = done ]
[ -f "$root/out/HTTP.ffpfsc" ]
if curl --compressed -fsS -H "X-WFM-Token: $token" -X POST "http://127.0.0.1:$port/api/convert?source=%2Ftmp%2F..%2Fetc&destination=$(printf '%s' "$root/out" | sed 's|/|%2F|g')&name=BAD.ffpfsc" >/tmp/mkpfs-http-invalid.out 2>/dev/null; then
  echo 'invalid source unexpectedly accepted' >&2; exit 1
fi
printf 'http smoke passed\n'
