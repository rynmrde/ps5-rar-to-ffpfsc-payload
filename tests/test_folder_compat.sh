#!/bin/sh
set -eu
ROOT=${MKPFS_UPSTREAM_ROOT:-}
if [ -z "$ROOT" ] || [ ! -f "$ROOT/mkpfs/__main__.py" ]; then
  echo "MKPFS_UPSTREAM_ROOT is not set; skipping upstream folder compatibility test" >&2
  exit 0
fi
TMP=${TMPDIR:-/tmp}/mkpfs-folder-compat.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/source/sce_sys/subdir" "$TMP/out"
printf '{"titleId":"PTEST0001"}' > "$TMP/source/sce_sys/param.json"
printf 'native-folder-test\n' > "$TMP/source/eboot.bin"
printf 'nested-content\n' > "$TMP/source/sce_sys/subdir/readme.txt"
./tools/mkpfs-convert-folder "$TMP/source" "$TMP/out" PTEST0001.ffpfsc
PYTHONPATH="$ROOT" python3 -m mkpfs verify "$TMP/out/PTEST0001.ffpfsc" > "$TMP/report.txt"
grep -q 'Warnings:.*0' "$TMP/report.txt"
grep -q 'Errors:.*0' "$TMP/report.txt"
cat "$TMP/report.txt"
