#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
bin=${WFM_SERVER_BIN:-./web-file-mgr-linux}
token=mkpfs-default-port-token-012345678

run_check() {
  requested=$1
  expected=$2
  log=$(mktemp /tmp/mkpfs-default-port.XXXXXX)
  if [ -n "$requested" ]; then
    WFM_PORT="$requested" WFM_ACCESS_TOKEN="$token" stdbuf -oL "$bin" >"$log" 2>&1 &
  else
    WFM_ACCESS_TOKEN="$token" stdbuf -oL "$bin" >"$log" 2>&1 &
  fi
  pid=$!
  cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$log"
  }
  trap cleanup EXIT INT TERM

  for _ in $(seq 1 80); do
    if grep -qx "listening on port $expected" "$log"; then
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      cat "$log" >&2
      exit 1
    fi
    sleep 0.1
  done
  grep -qx "listening on port $expected" "$log"
  curl --compressed -fsS "http://127.0.0.1:$expected/" >/dev/null
  cleanup
  trap - EXIT INT TERM
}

run_check '' 8888
run_check 18888 18888
run_check 0 8888

# Hold a configured first-choice port with one payload instance.  The second
# instance must perform the same sequential fallback as the default 8888 flow.
first_log=$(mktemp /tmp/mkpfs-port-fallback-first.XXXXXX)
second_log=$(mktemp /tmp/mkpfs-port-fallback-second.XXXXXX)
WFM_PORT=18890 WFM_ACCESS_TOKEN="$token" stdbuf -oL "$bin" >"$first_log" 2>&1 &
first_pid=$!
second_pid=
cleanup_fallback() {
  kill "${second_pid:-}" "${first_pid:-}" 2>/dev/null || true
  wait "${second_pid:-}" "${first_pid:-}" 2>/dev/null || true
  rm -f "$first_log" "$second_log"
}
trap cleanup_fallback EXIT INT TERM

for _ in $(seq 1 80); do
  grep -qx 'listening on port 18890' "$first_log" && break
  sleep 0.1
done
grep -qx 'listening on port 18890' "$first_log"

WFM_PORT=18890 WFM_ACCESS_TOKEN="$token" stdbuf -oL "$bin" >"$second_log" 2>&1 &
second_pid=$!
for _ in $(seq 1 80); do
  grep -qx 'listening on port 18891' "$second_log" && break
  sleep 0.1
done
grep -qx 'listening on port 18891' "$second_log"
curl --compressed -fsS "http://127.0.0.1:18891/" >/dev/null
cleanup_fallback
trap - EXIT INT TERM

printf '%s\n' 'DEFAULT_PORT_8888_AND_FALLBACK_PASS'
