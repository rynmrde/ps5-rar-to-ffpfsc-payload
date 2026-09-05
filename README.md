# MkPFS PS5

This repository integrates the PS5 Web File Manager with a native implementation of the upstream MkPFS folder conversion pipeline. It preserves the original file browser, embedded HTTP server, background file tasks, responsive English/Chinese UI, startup notification, and Home Screen launcher flow.

## Completed conversion pipeline

The native implementation now supports the complete host-tested pipeline:

> **source folder → native exFAT image → four-inode PS5 PFS wrapper → upstream-compatible PFSC stream → `.ffpfsc`**

The PFSC and PFS stages from the previous verified revision were preserved. The new exFAT stage uses the upstream deterministic layout: main and backup boot regions, aligned FAT, allocation bitmap, exact upstream up-case table and checksum, root metadata entries, UTF-16 directory-entry sets, contiguous cluster allocation, nested-directory recursion, streamed file payloads, and title-ID-derived embedded names from `sce_sys/param.json` when available.

The folder serializer uses bounded memory for directory entries and a 1 MiB file-data buffer. It does not load a complete folder or complete file into memory. Output stages use temporary paths and atomic rename. Cancellation is checked during file emission and PFSC processing. PFSC block compression supports a configurable worker count with Auto mode; workers compress independent 64 KiB blocks in a bounded pool while the main thread writes blocks in deterministic order. Serial and parallel outputs are byte-identical on the same fixture.

## Web File Manager integration

The UI has a **Convert folder** action. Select one folder in the existing file browser, navigate to the desired destination directory, choose the output filename and compression level, and start the conversion. The new `/api/convert` endpoint performs source/destination validation, title-safe filename validation, target-space preflight, task queuing, progress reporting, speed/ETA tracking through the existing task API, cancellation, and final output reporting. Completed conversion tasks appear in the existing task history and task overlay.

The host-only tools are also available for reproducible testing:

```sh
make test-native
make mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder
make compat-upstream MKPFS_UPSTREAM_ROOT=/path/to/MkPFS
make linux
```

Direct conversion from a folder is:

```sh
./tools/mkpfs-convert-folder SOURCE_DIR DEST_DIR OUTPUT.ffpfsc [WORKERS|auto]
```

The direct exFAT stage is:

```sh
./tools/mkpfs-exfat SOURCE_DIR OUTPUT.exfat
```

The upstream MkPFS verifier can check a generated `.ffpfsc`:

```sh
python3 -m mkpfs verify OUTPUT.ffpfsc
```

The repository’s `tests/test_folder_compat.sh` creates a real nested fixture and requires the upstream verifier to report `Warnings: 0` and `Errors: 0` when `MKPFS_UPSTREAM_ROOT` is set.

## Verification status

The complete host matrix passes. A nested real-folder fixture containing `sce_sys/param.json`, `eboot.bin`, and `sce_sys/subdir/readme.txt` generated a `.ffpfsc` that the upstream MkPFS verifier accepted with zero warnings and zero errors. Upstream tree inspection of the native raw exFAT stage shows the expected nested directory and file names. The benchmark is documented in [BENCHMARKS.md](BENCHMARKS.md).

The benchmark is a host smoke test, not a PS5 performance claim. In the final regression on the same sparse 256 MiB fixture, serial mode took 1.578249 seconds (162.21 MiB/s), four workers took 0.692410 seconds (369.72 MiB/s), and Auto mode took 0.622651 seconds (411.15 MiB/s). All three outputs were byte-identical and passed upstream verification with zero warnings and zero errors. The implementation is designed for bounded memory and streaming, but 50–100 GiB target measurements require a suitable storage and target environment.

## PS5 build status

The target build is verified with the public `ps5-payload-dev/sdk` checkout after performing its documented install into a real SDK prefix and building target-compatible dependencies:

```sh
make DESTDIR=/path/to/ps5-payload-sdk install
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

The original checkout was source-only and lacked `target/include` and `target/lib`; its `include/freebsd/ctype.h` was therefore outside the wrapper’s expected sysroot. The documented SDK install generated the target headers, CRT objects, linker scripts, libc, pthread library, and SCE stub libraries. Target zlib 1.3.1 and libmicrohttpd were then built into the target homebrew prefix. The resulting `web-file-mgr.elf` is a stripped x86-64 PS5 payload ELF with no unresolved symbols and the expected `.sprx` dependencies. It is packaged in the `v0.2.0-alpha.2` prerelease.

## Safety-sensitive runtime behavior

On PS5, the HTTP/API behavior follows the upstream Web File Manager and does
not require a browser-supplied token. The server still limits connections,
request body size, and idle connection time; host builds retain the token gate
for local security regression tests.

Normal PS5 startup follows the upstream launcher flow: it checks for the
managed `FMGR88888` application under `/user/app`, creates only missing
metadata/icon files, and invokes the upstream PS5 application-install API.
Existing launcher metadata, including unrelated title IDs, is never
overwritten. The upstream `param.json` metadata uses the standard port 8888
deeplink. The `/api/roots` diagnostic endpoint
reports which of `/`, `/user/app`, `/data`, `/mnt`, USB, and extended-storage
paths are readable; the UI uses it only when the root listing is empty to find
a usable mounted storage root. Conversion rejects
symlinked sources, traversal components, case-folding collisions, oversized
metadata allocations, and implicit replacement of an existing output file.

## Licensing

The project remains GPLv3-or-later. MkPFS is GPLv3-or-later, and libmicrohttpd is LGPL. Original notices and third-party attribution are preserved in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
