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

run_check '' 6777
run_check 16777 16777
run_check 0 6777
printf '%s\n' 'DEFAULT_PORT_6777_PASS'
