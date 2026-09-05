#!/bin/sh
set -eu
root=${1:-/tmp/mkpfs-worker-bench}
base=${2:-/tmp/mkpfs-worker-bench-out}
rm -rf "$root" "$base"
mkdir -p "$root" "$base"
truncate -s 268435456 "$root/zero.bin"
for workers in 1 4 auto; do
  out="$base/$workers"
  mkdir -p "$out"
  start=$(date +%s%N)
  ./tools/mkpfs-convert-folder "$root" "$out" BENCH.ffpfsc "$workers" > "$out/log.txt"
  end=$(date +%s%N)
  elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.6f", (e-s)/1000000000 }')
  output="$out/BENCH.ffpfsc"
  size=$(stat -c %s "$output")
  hash=$(sha256sum "$output" | awk '{print $1}')
  printf 'workers=%s elapsed_seconds=%s bytes=%s sha256=%s\n' "$workers" "$elapsed" "$size" "$hash"
done
