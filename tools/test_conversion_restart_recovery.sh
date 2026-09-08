#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
bin=${WFM_SERVER_BIN:-./web-file-mgr-linux}
root=/tmp/mkpfs-restart-recovery
source=$root/source
destination=$root/destination
journals=$root/journals
port=18091
token=mkpfs-restart-recovery-token-012345
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
printf '{"titleId":"PRSM00001"}\n' > "$source/sce_sys/param.json"
# Incompressible data ensures the PFSC stage exceeds a durable 128-block
# checkpoint before the simulated abrupt payload loss.
dd if=/dev/urandom of="$source/data.bin" bs=1M count=16 status=none

start() {
  WFM_PORT=$port WFM_ACCESS_TOKEN=$token WFM_RESUME_DIR=$journals \
    stdbuf -oL -eL "$bin" >"$root/server.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 100); do
    if curl -fsS -H "X-WFM-Token: $token" "http://127.0.0.1:$port/api/tasks" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  cat "$root/server.log" >&2 || true
  return 1
}

start
response=$(curl -fsS -X POST -H "X-WFM-Token: $token" \
  "http://127.0.0.1:$port/api/convert?source=$source&destination=$destination&name=resume.ffpfsc&profile=7&workers=1")
printf '%s' "$response" | grep -q '"ok":true'

# Simulate an abrupt payload loss only after a durable PFSC checkpoint exists.
for _ in $(seq 1 600); do
  if find "$destination" -type f -name '.resume.ffpfsc.mkpfs-*.pfs.stage' \
    -size +8192k | grep -q .; then
    break
  fi
  sleep 0.05
done
find "$destination" -type f -name '.resume.ffpfsc.mkpfs-*.pfs.stage' \
  -size +8192k | grep -q .
journal=$(find "$journals" -type f -name 'mkpfs-conversion-*.resume' -size +100c -print -quit)
test -n "$journal"
# conversion_journal_t has a fixed 8+4+4+4+4+PATH_MAX+PATH_MAX+NAME_MAX
# prefix, followed by natural 8-byte alignment and the resume-state phase.
# On the Linux host exercised by this test, that phase is byte 8472.  A value
# of 2 confirms pack_pfsc_into_stream synchronized its checkpoint before the
# process is deliberately terminated.
phase=$(od -An -tu4 -j 8472 -N 4 "$journal" | tr -d '[:space:]')
test "$phase" = 2
kill -KILL "$pid"
wait "$pid" 2>/dev/null || true
pid=
# A completed exFAT snapshot and persisted inner filename are sufficient for
# PFSC verification and PFS publication; recovery must not re-walk the source.
mv "$source" "$root/source.offline"

# The startup scanner must automatically restore the one valid journal.
start
for _ in $(seq 1 1200); do
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
printf 'conversion restart recovery test passed\n'
