#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
bin=${WFM_SERVER_BIN:-./web-file-mgr-linux}
root=/tmp/mkpfs-exfat-restart-recovery
source=$root/source
destination=$root/destination
journals=$root/journals
port=18092
token=mkpfs-exfat-restart-token-0123
pid=

cleanup() {
  if [ -n "${pid:-}" ]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$root"
}
trap cleanup EXIT INT TERM
rm -rf "$root"
mkdir -p "$source/sce_sys" "$destination" "$journals"
printf '{"titleId":"EXRS00001"}\n' > "$source/sce_sys/param.json"
# Two zero-filled files force a durable exFAT checkpoint after the first whole
# file.  The stage is intentionally large enough that the test can terminate
# the payload while it is still in the exFAT phase.
dd if=/dev/zero of="$source/0-large.bin" bs=1M count=384 status=none
dd if=/dev/zero of="$source/1-large.bin" bs=1M count=384 status=none

start() {
  WFM_PORT=$port WFM_ACCESS_TOKEN=$token WFM_RESUME_DIR=$journals \
    stdbuf -oL -eL "$bin" >"$root/server.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 200); do
    if curl -fsS -H "X-WFM-Token: $token" \
      "http://127.0.0.1:$port/api/tasks" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  cat "$root/server.log" >&2 || true
  return 1
}

stage=$(find "$destination" -type f -name '.resume.ffpfsc.mkpfs-*.exfat.stage' -print -quit || true)
start
response=$(curl -fsS -X POST -H "X-WFM-Token: $token" \
  "http://127.0.0.1:$port/api/convert?source=$source&destination=$destination&name=resume.ffpfsc&profile=7&workers=1")
printf '%s' "$response" | grep -q '"ok":true'

# Wait until the durable exFAT checkpoint has written the first 384 MiB file,
# while PFSC creation has not begun.  `du` detects written blocks rather than
# merely the preallocated logical image length.
for _ in $(seq 1 1200); do
  stage=$(find "$destination" -type f -name '.resume.ffpfsc.mkpfs-*.exfat.stage' -print -quit || true)
  pfs=$(find "$destination" -type f -name '.resume.ffpfsc.mkpfs-*.pfs.stage' -print -quit || true)
  if [ -n "$stage" ] && [ -z "$pfs" ] && [ "$(du -k "$stage" | awk '{print $1}')" -ge 262144 ]; then
    break
  fi
  sleep 0.05
done
test -n "$stage"
test ! -e "$destination/resume.ffpfsc"
test -z "${pfs:-}"
test "$(du -k "$stage" | awk '{print $1}')" -ge 262144
journal=$(find "$journals" -type f -name 'mkpfs-conversion-*.resume' -size +100c -print -quit)
test -n "$journal"

kill -KILL "$pid"
wait "$pid" 2>/dev/null || true
pid=

# Recovery must rescan the still-available source, validate its metadata
# fingerprint, and continue from the last fully checkpointed source file.
start
for _ in $(seq 1 2400); do
  tasks=$(curl -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$port/api/tasks")
  if printf '%s' "$tasks" | grep -q '"state":"done"'; then
    break
  fi
  if printf '%s' "$tasks" | grep -q '"state":"failed"'; then
    printf '%s\n' "$tasks" >&2
    exit 1
  fi
  sleep 0.05
done
printf '%s' "$tasks" | grep -q '"state":"done"'
test -f "$destination/resume.ffpfsc"
test ! -e "$destination/.resume.ffpfsc.mkpfs-conversion.incomplete"
test -z "$(find "$journals" -type f -name 'mkpfs-conversion-*.resume' -print -quit)"
printf 'exFAT restart recovery test passed\n'
