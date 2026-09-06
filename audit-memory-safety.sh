#!/usr/bin/env bash
# Host-only ASan/UBSan and Valgrind execution for release audit.
set -Eeuo pipefail
repo=/home/ubuntu/audits/mkpfs-ps5-upstream-review
cd "$repo"
mkdir -p audit-bin audit-logs

printf '%s\n' '== ASan/UBSan native tools =='
SAN='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Werror -Isrc'
clang $SAN -o audit-bin/test_mkpfs_native_asan tests/test_mkpfs_native.c src/mkpfs_native.c -lz -pthread
clang $SAN -o audit-bin/mkpfs-convert-folder_asan tools/mkpfs-convert-folder.c src/mkpfs_native.c -lz -pthread
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./audit-bin/test_mkpfs_native_asan

printf '%s\n' '== ASan/UBSan full Linux server =='
clang $SAN $(pkg-config --cflags libmicrohttpd) \
  -DVERSION_TAG='"v1.7"' -DTITLE_ID='"FMGR88888"' \
  -o audit-bin/web-file-mgr-linux_asan \
  src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c src/upload.c \
  src/download.c src/text.c src/list.c src/space.c src/fs_util.c src/json_util.c \
  src/path_util.c src/asset.c src/mime.c src/notify.c src/pkg_installer.c src/pkg_info.c \
  src/mkpfs_native.c gen/*.c $(pkg-config --libs libmicrohttpd) -pthread -lz
WFM_PORT=18080 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  python3 ./audit-http-robustness.py ./audit-bin/web-file-mgr-linux_asan ./audit-logs/http-asan.log

printf '%s\n' '== Standard HTTP conversion smoke =='
WFM_PORT=18080 ./tools/smoke_http.sh

printf '%s\n' '== Valgrind native test =='
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
  --errors-for-leak-kinds=definite --error-exitcode=99 \
  ./tests/test_mkpfs_native 2>&1 | tee audit-logs/valgrind-native.log

printf '%s\n' 'MEMORY_SAFETY_TESTS_PASS'
