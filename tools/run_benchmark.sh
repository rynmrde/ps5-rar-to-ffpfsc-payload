#!/bin/sh
set -eu
root=${1:-/tmp/mkpfs-bench}
out=${2:-/tmp/mkpfs-bench-out}
rm -rf "$root" "$out"
mkdir -p "$root" "$out"
truncate -s 268435456 "$root/zero.bin"
start=$(date +%s%N)
./tools/mkpfs-convert-folder "$root" "$out" BENCH.ffpfsc > /tmp/mkpfs-bench-output.txt
end=$(date +%s%N)
cat /tmp/mkpfs-bench-output.txt
printf 'elapsed_seconds=%.6f\n' "$(awk -v s="$start" -v e="$end" 'BEGIN { printf (e-s)/1000000000 }')"
ls -l "$out/BENCH.ffpfsc"
