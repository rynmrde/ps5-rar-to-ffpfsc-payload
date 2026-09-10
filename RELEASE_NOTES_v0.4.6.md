# Release Notes v0.4.6

## Summary
Release v0.4.6 fixes 12 bugs across the ps5-rar-to-ffpfsc-payload codebase, with critical fixes for exFAT compatibility, Linux build stability, tar corruption, and memory safety.

## Bug Fixes

### H1: _GNU_SOURCE missing — Linux build broken
**File:** `Makefile`
**Problem:** `src/text.c` uses `asprintf()` and `src/filemgr.c` uses `strcasestr()`, which are GNU extensions requiring `_GNU_SOURCE` on glibc. Without it, `make linux` fails under `-Werror`.
**Fix:** Added `-D_GNU_SOURCE` to both `CFLAGS` and `LINUX_CFLAGS`.

### H2: Upload to exFAT/FAT always fails
**File:** `src/upload.c` — `filemgr_upload_finish()`
**Problem:** The `else` branch called `link()` which returns `EPERM` on exFAT, causing uploads to FAT/exFAT partitions to always fail at the final step.
**Fix:** Replaced the else branch with a fallback pattern: try `link()` first, and if it fails with `EOPNOTSUPP`/`EPERM`/`EXDEV`, fall back to `rename()`.

### H3: Conversion publish fails on exFAT
**File:** `src/mkpfs_native.c` — `publish_stage_no_replace()`
**Problem:** Same as H2 — `link()` fails on exFAT, causing conversion to fail at the final step after hours of work.
**Fix:** Replaced the entire function with a version that handles the exFAT fallback via `rename()`.

### M1: Cancel queued download hangs
**File:** `src/filemgr.c` — `api_cancel()`
**Problem:** `api_cancel()` set `cancel_requested` but did not broadcast `g_url_download_slot`. A queued download in `pthread_cond_wait()` would never wake up.
**Fix:** Added `pthread_cond_broadcast(&g_url_download_slot)` after unlocking `g_tasks_lock` when a download was found.

### M2: tar download >= 8 GiB silently corrupts
**File:** `src/download.c` — `tar_queue_header()`
**Problem:** The ustar size field is 11 octal digits max (`077777777777` = 8,589,934,591 bytes). Files >= 8 GiB were silently truncated.
**Fix:** Added a check: `if (size > 077777777777ULL) { errno = EFBIG; return -1; }`

### L1: api_pkg_icon memory leak
**File:** `src/pkg_info.c` — `api_pkg_icon()`
**Problem:** If `malloc` succeeded but `read_at` failed, the allocated `icon` buffer was never freed.
**Fix:** Changed `unsigned char *icon;` to `unsigned char *icon = NULL;` and added `free(icon);` as the first line in the error block.

### L2: app_installer.c missing stdlib.h
**File:** `src/app_installer.c`
**Problem:** Missing `#include <stdlib.h>` caused implicit function declaration warnings/errors under strict compilation.
**Fix:** Added `#include <stdlib.h>` after `#include <stdint.h>`.

### L3: api_convert accepts '/' in name
**File:** `src/filemgr.c` — `api_convert()`
**Problem:** The function did not validate that the conversion name contained no `/` characters, unlike `api_extract` and `api_url_download`.
**Fix:** Added `strchr(name, '/') ||` to the validation condition.

### L4: api_rename silently overwrites
**File:** `src/filemgr.c` — `api_rename()`
**Problem:** The function called `rename()` without checking if the target already existed, silently overwriting files.
**Fix:** Added an `lstat()` check before `rename()` that returns `MHD_HTTP_CONFLICT` if the target already exists.

### L5: Copy/move overwrite=0 TOCTOU
**File:** `src/filemgr_internal.h` — `file_task_t` struct
**Fix:** Added `int allow_overwrite;` field after `int recursive`.

**File:** `src/filemgr.c` — `create_task_response()`
**Fix:** Added `task->allow_overwrite = overwrite;` after `task->recursive = recursive;`.

**File:** `src/filemgr.c` — `copy_file_buffered()` and `copy_file_pipeline()`
**Fix:** Replaced bare `rename()` call with the `link()`+`rename()` fallback pattern (same as H2/H3) when `task->allow_overwrite` is 0.

### SPEED OVERFLOW: task_update overflows for large transfers
**File:** `src/task.c` — `task_update()`
**Problem:** `delta * 1000000000ULL` overflows `uint64` when `delta > ~18.4 GB`.
**Fix:** Changed to `(long double)` arithmetic: `(long double)delta * 1000000000.0L / (long double)elapsed_ns`.

## Version Bump
**File:** `Makefile`
**Change:** `VERSION_TAG := v0.4.5` → `VERSION_TAG := v0.4.6`

## Build Instructions

### PS5 ELF
```bash
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean && make all
```

### Linux binary (proves H1)
```bash
make linux
```

### Run tests
```bash
make test-native && make test-resume
```

### Verify
```bash
ls -la rar-to-ffpfsc-ps5-payload.elf
sha256sum rar-to-ffpfsc-ps5-payload.elf
```

## Release Artifacts
- `SHA256SUMS-v0.4.6.txt` — SHA-256 checksums of the PS5 ELF
- `RELEASE_NOTES_v0.4.6.md` — These release notes
