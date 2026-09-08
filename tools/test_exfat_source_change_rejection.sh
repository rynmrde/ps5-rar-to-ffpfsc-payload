#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
bin=${WFM_SERVER_BIN:-./web-file-mgr-linux}
root=/tmp/mkpfs-exfat-source-change
source=$root/source
destination=$root/destination
journals=$root/journals
port=18093
token=mkpfs-exfat-change-token-01234
pid=

cleanup() {
  if [ -n "${pid:-}" ]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  if [ "${DEBUG_KEEP_ROOT:-0}" != 1 ]; then
    rm -rf "$root"
  fi
}
trap cleanup EXIT INT TERM
rm -rf "$root"
mkdir -p "$source/sce_sys" "$destination" "$journals"
printf '{"titleId":"EXCH00001"}\n' > "$source/sce_sys/param.json"
dd if=/dev/zero of="$source/0-large.bin" bs=1M count=192 status=none
dd if=/dev/zero of="$source/1-large.bin" bs=1M count=192 status=none

start() {
  WFM_PORT=$port WFM_ACCESS_TOKEN=$token WFM_RESUME_DIR=$journals \
    stdbuf -oL -eL "$bin" >"$root/server.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 200); do
    if curl -fsS -H "X-WFM-Token: $token" \
      "http://127.0.0.1:$port/api/roots" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  cat "$root/server.log" >&2 || true
  return 1
}

start
curl -fsS -X POST -H "X-WFM-Token: $token" \
  "http://127.0.0.1:$port/api/convert?source=$source&destination=$destination&name=changed.ffpfsc&profile=7&workers=1" | grep -q '"ok":true'
for _ in $(seq 1 1200); do
  stage=$(find "$destination" -type f -name '.changed.ffpfsc.mkpfs-*.exfat.stage' -print -quit || true)
  journal=$(find "$journals" -type f -name 'mkpfs-conversion-*.resume' -size +100c -print -quit || true)
  if [ -n "$stage" ] && [ -n "$journal" ]; then
    break
  fi
  sleep 0.05
done
test -n "${stage:-}"
test -n "${journal:-}"
kill -KILL "$pid"
wait "$pid" 2>/dev/null || true
pid=

# A changed source must never be mixed with already checkpointed exFAT data.
printf 'changed after durable checkpoint\n' >> "$source/0-large.bin"
start
for _ in $(seq 1 400); do
  tasks=$(curl -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$port/api/tasks")
  if printf '%s' "$tasks" | grep -q '"state":"failed"'; then
    break
  fi
  sleep 0.05
done
printf '%s' "$tasks" | grep -q '"state":"failed"'
if ! printf '%s' "$tasks" | grep -q '"error_code":"conversion_source_changed"' &&
   ! printf '%s' "$tasks" | grep -q '"error_code":"resume_checkpoint_invalid"'; then
  printf '%s\n' "$tasks" >&2
  exit 1
fi
test ! -e "$destination/changed.ffpfsc"
test -z "$(find "$destination" -type f -name '.changed.ffpfsc.mkpfs-*.stage' -print -quit)"
test ! -e "$destination/.changed.ffpfsc.mkpfs-conversion.incomplete"
test -z "$(find "$journals" -type f -name 'mkpfs-conversion-*.resume' -print -quit)"
printf 'changed-source exFAT recovery rejection test passed\n'
