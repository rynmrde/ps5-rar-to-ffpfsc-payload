#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-/tmp/mkpfs-finalization-benchmark}
count=${COUNT:-32000}
directories=${DIRECTORIES:-256}
workers=${WORKERS:-1}
base_commit=${BASE_COMMIT:-93fd30c}
payload=${PAYLOAD:-compressible}

if [ "$count" -lt 1 ] || [ "$directories" -lt 1 ] || [ "$workers" -gt 8 ] ||
   { [ "$payload" != compressible ] && [ "$payload" != incompressible ]; }; then
  echo 'COUNT and DIRECTORIES must be positive; WORKERS must be 0 through 8; PAYLOAD must be compressible or incompressible' >&2
  exit 2
fi

rm -rf "$work"
mkdir -p "$work/source/sce_sys" "$work/before-out" "$work/after-out"
printf '{"titleId":"PTEST0001"}\n' > "$work/source/sce_sys/param.json"

# Create a wide, nested, many-small-files tree. A 64 KiB exFAT cluster is
# allocated for every file; this makes finalization exercise PFSC compression,
# offset-table generation, validation, and final PFS assembly instead of source
# bandwidth alone.
i=0
while [ "$i" -lt "$directories" ]; do
  mkdir -p "$work/source/content/dir-$(printf '%04d' "$i")"
  i=$((i + 1))
done

i=0
while [ "$i" -lt "$count" ]; do
  d=$((i % directories))
  path="$work/source/content/dir-$(printf '%04d' "$d")/file-$(printf '%06d' "$i").bin"
  if [ "$payload" = incompressible ]; then
    dd if=/dev/urandom of="$path" bs=65536 count=1 status=none
  else
    printf 'small-file-%08d-abcdefghijklmnopqrstuvwxyz-0123456789\n' "$i" > "$path"
  fi
  i=$((i + 1))
done

# The historical source is compiled independently to retain a true baseline.
git -C "$root" show "$base_commit:src/mkpfs_native.c" > "$work/mkpfs_native_before.c"
cc -O2 -Wall -Werror -I"$root/src" -o "$work/before" \
  "$root/tools/profile_finalization.c" "$work/mkpfs_native_before.c" -lz -pthread
cc -O2 -Wall -Werror -I"$root/src" -o "$work/after" \
  "$root/tools/profile_finalization.c" "$root/src/mkpfs_native.c" -lz -pthread

"$work/before" "$work/source" "$work/before-out" result.ffpfsc "$workers" \
  | tee "$work/before.txt"
"$work/after" "$work/source" "$work/after-out" result.ffpfsc "$workers" \
  | tee "$work/after.txt"

cmp "$work/before-out/result.ffpfsc" "$work/after-out/result.ffpfsc"
awk -F= '
  FILENAME ~ /before\.txt$/ { before[$1] = $2 }
  FILENAME ~ /after\.txt$/ { after[$1] = $2 }
  END {
    printf "files=%d\ndirectories=%d\nworkers=%d\npayload='"$payload"'\n", '"$count"', '"$directories"', '"$workers"'
    printf "before_finalizing_seconds=%s\nafter_finalizing_seconds=%s\n", before["finalizing_seconds"], after["finalizing_seconds"]
    printf "before_total_seconds=%s\nafter_total_seconds=%s\n", before["total_seconds"], after["total_seconds"]
    printf "finalizing_improvement_percent=%.2f\n", (before["finalizing_seconds"] - after["finalizing_seconds"]) * 100 / before["finalizing_seconds"]
    printf "total_improvement_percent=%.2f\n", (before["total_seconds"] - after["total_seconds"]) * 100 / before["total_seconds"]
    print "byte_identity=passed"
  }
' "$work/before.txt" "$work/after.txt" | tee "$work/summary.txt"
