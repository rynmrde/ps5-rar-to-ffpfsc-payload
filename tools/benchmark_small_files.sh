#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-/tmp/mkpfs-small-file-benchmark}
count=${COUNT:-4000}
runs=${RUNS:-5}
base_commit=${BASE_COMMIT:-d36b83f}
rm -rf "$work"
mkdir -p "$work/src" "$work/before-out" "$work/after-out"

# Fixed 64-byte payloads ensure the workload is dominated by per-file format
# work and exFAT zero-padding rather than source read bandwidth.
i=0
while [ "$i" -lt "$count" ]; do
  printf 'small-file-%08d-abcdefghijklmnopqrstuvwxyz-0123456789\n' "$i" > "$work/src/file-$(printf '%05d' "$i").txt"
  i=$((i + 1))
done

git -C "$root" show "$base_commit:src/mkpfs_native.c" > "$work/mkpfs_native_before.c"
cc -O2 -Wall -Werror -I"$root/src" -o "$work/before" \
  "$root/tools/mkpfs-convert-folder.c" "$work/mkpfs_native_before.c" -lz -pthread
cc -O2 -Wall -Werror -I"$root/src" -o "$work/after" \
  "$root/tools/mkpfs-convert-folder.c" "$root/src/mkpfs_native.c" -lz -pthread

: > "$work/timings.txt"
i=1
while [ "$i" -le "$runs" ]; do
  /usr/bin/time -f 'before %e' "$work/before" "$work/src" "$work/before-out" "run-$i.ffpfsc" 1 2>> "$work/timings.txt"
  /usr/bin/time -f 'after  %e' "$work/after" "$work/src" "$work/after-out" "run-$i.ffpfsc" 1 2>> "$work/timings.txt"
  i=$((i + 1))
done
cmp "$work/before-out/run-1.ffpfsc" "$work/after-out/run-1.ffpfsc"
awk '
  $1 == "before" { before += $2; before_n++ }
  $1 == "after" { after += $2; after_n++ }
  END {
    b = before / before_n; a = after / after_n;
    printf "files=%d runs=%d\nbefore_mean_seconds=%.6f\nafter_mean_seconds=%.6f\nimprovement_percent=%.2f\nbyte_identity=passed\n", '"$count"', before_n, b, a, (b-a)*100/b;
  }
' "$work/timings.txt" | tee "$work/summary.txt"
