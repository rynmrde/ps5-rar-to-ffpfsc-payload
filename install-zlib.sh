#!/usr/bin/env bash
# Builds static zlib into the PS5 payload SDK sysroot when the SDK distribution lacks it.
set -euo pipefail

ZLIB_VERSION="${ZLIB_VERSION:-1.3.1}"
ZLIB_URL="${ZLIB_URL:-https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz}"
ZLIB_TARBALL="${ZLIB_TARBALL:-}"

if [[ -z "${PS5_PAYLOAD_SDK:-}" ]]; then
  echo "error: PS5_PAYLOAD_SDK is not set" >&2
  exit 1
fi

source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"

zlib_header="${PS5_SYSROOT}/include/zlib.h"
zlib_library="${PS5_SYSROOT}/lib/libz.a"
if [[ -f "${zlib_header}" && -f "${zlib_library}" ]]; then
  echo "zlib is already available in PS5_PAYLOAD_SDK"
  exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf -- "${tmpdir}"' EXIT
archive="${tmpdir}/zlib-${ZLIB_VERSION}.tar.gz"

if [[ -n "${ZLIB_TARBALL}" ]]; then
  cp "${ZLIB_TARBALL}" "${archive}"
elif command -v wget >/dev/null 2>&1; then
  wget -O "${archive}" "${ZLIB_URL}"
elif command -v curl >/dev/null 2>&1; then
  curl -L -o "${archive}" "${ZLIB_URL}"
else
  echo "error: install wget or curl, or set ZLIB_TARBALL" >&2
  exit 1
fi

tar xf "${archive}" -C "${tmpdir}"
cd "${tmpdir}/zlib-${ZLIB_VERSION}"

export CFLAGS="${CFLAGS:-} -O2 -fPIC"
./configure --static --prefix="${PS5_SYSROOT}"
"${MAKE:-make}"
# prospero.sh sets DESTDIR to the sysroot for autotools projects. zlib's
# --prefix above is already absolute, so clear DESTDIR to avoid a nested path.
DESTDIR= "${MAKE:-make}" install

test -f "${zlib_header}"
test -f "${zlib_library}"
echo "zlib installed for ps5-payload-sdk"
