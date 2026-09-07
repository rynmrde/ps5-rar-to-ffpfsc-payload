#!/usr/bin/env bash
# Host-only ASan/UBSan and Valgrind execution for release audit.
set -Eeuo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$repo"
mkdir -p audit-bin audit-logs

printf '%s\n' '== ASan/UBSan native tools =='
SAN='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Werror -Isrc'
SAN_CXX='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wall'
clang-18 $SAN -o audit-bin/test_mkpfs_native_asan tests/test_mkpfs_native.c src/mkpfs_native.c -lz -pthread
clang-18 $SAN -o audit-bin/mkpfs-convert-folder_asan tools/mkpfs-convert-folder.c src/mkpfs_native.c -lz -pthread
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./audit-bin/test_mkpfs_native_asan

printf '%s\n' '== ASan/UBSan full Linux server =='
make -C third_party/unrar-ps5 clean
make -C third_party/unrar-ps5 library CC=clang-18 CXX=clang++-18 AR=ar \
  CFLAGS="$SAN_CXX" CXXFLAGS="$SAN_CXX -std=c++11 -Wno-logical-op-parentheses -Wno-switch -Wno-dangling-else -Wno-unused-parameter -Wno-reorder"
clang++-18 -x c $SAN -o audit-bin/test_archive_extract_asan tests/test_archive_extract.c \
  -x none -Wl,--whole-archive third_party/unrar-ps5/libmkpfsarchive.a -Wl,--no-whole-archive -pthread -lstdc++
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ARCHIVE_TEST_BIN="$repo/audit-bin/test_archive_extract_asan" ./tests/test_archive_extract.sh
clang++-18 -x c $SAN $(pkg-config --cflags libmicrohttpd) \
  -DVERSION_TAG='"v0.3.1"' -DTITLE_ID='"FMGR88888"' \
  -o audit-bin/web-file-mgr-linux_asan \
  src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c src/upload.c \
  src/download.c src/url_download.c src/text.c src/list.c src/space.c src/fs_util.c src/json_util.c \
  src/path_util.c src/asset.c src/mime.c src/notify.c src/pkg_installer.c src/pkg_info.c \
  src/mkpfs_native.c gen/*.c -x none -Wl,--whole-archive third_party/unrar-ps5/libmkpfsarchive.a -Wl,--no-whole-archive \
  $(pkg-config --libs libmicrohttpd) -pthread -lz -lstdc++
WFM_PORT=18080 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  python3 ./audit-http-robustness.py ./audit-bin/web-file-mgr-linux_asan ./audit-logs/http-asan.log

WFM_SERVER_BIN="$repo/audit-bin/web-file-mgr-linux_asan" WFM_PORT=18084 \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./tools/test_url_download_http.sh

printf '%s\n' '== Standard HTTP conversion smoke =='
WFM_PORT=18080 ./tools/smoke_http.sh

printf '%s\n' '== Valgrind native test =='
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
  --errors-for-leak-kinds=definite --error-exitcode=99 \
  ./tests/test_mkpfs_native 2>&1 | tee audit-logs/valgrind-native.log

printf '%s\n' 'MEMORY_SAFETY_TESTS_PASS'
