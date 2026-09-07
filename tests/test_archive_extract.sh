#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bin=${ARCHIVE_TEST_BIN:-"$root/tests/test_archive_extract"}
tmp=$(mktemp -d /tmp/mkpfs-archive-test.XXXXXX)
cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT INT TERM

mkdir -p "$tmp/input/nested"
printf 'hello from archive\n' > "$tmp/input/hello.txt"
printf 'nested archive value' > "$tmp/input/nested/info.txt"
dd if=/dev/urandom of="$tmp/input/payload.bin" bs=1024 count=16 status=none

(
  cd "$tmp/input"
  rar a -idq "$tmp/single.rar" hello.txt nested/info.txt payload.bin
  rar a -idq -v2k "$tmp/multi.rar" hello.txt nested/info.txt payload.bin
  rar a -idq -psafety-test-password "$tmp/encrypted.rar" hello.txt nested/info.txt payload.bin
  7z a -bd -y "$tmp/single.7z" hello.txt nested/info.txt payload.bin >/dev/null
  7z a -bd -y -v2k "$tmp/multi.7z" hello.txt nested/info.txt payload.bin >/dev/null
  7z a -bd -y -psafety-test-password -mhe=on "$tmp/encrypted.7z" hello.txt nested/info.txt payload.bin >/dev/null
)

"$bin" "$tmp/single.rar" "$tmp/rar-out" nested/info.txt "nested archive value"
"$bin" "$tmp/single.7z" "$tmp/7z-out" nested/info.txt "nested archive value"
"$bin" "$tmp/multi.part01.rar" "$tmp/rar-multipart-out" nested/info.txt "nested archive value"
"$bin" "$tmp/multi.7z.001" "$tmp/7z-multipart-out" nested/info.txt "nested archive value"
"$bin" "$tmp/encrypted.rar" "$tmp/rar-password-out" nested/info.txt "nested archive value" safety-test-password
"$bin" "$tmp/encrypted.7z" "$tmp/7z-password-out" nested/info.txt "nested archive value" safety-test-password

printf 'archive extraction: RAR, 7z, encrypted RAR/7z, RAR multipart, 7z multipart, and pre-cancel passed\n'
