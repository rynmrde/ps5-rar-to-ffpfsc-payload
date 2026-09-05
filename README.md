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

The benchmark is a host smoke test, not a PS5 performance claim. On the same sparse 256 MiB fixture, serial mode took 1.586118 seconds (161.40 MiB/s), four workers took 0.763786 seconds (335.17 MiB/s), and Auto mode took 0.812662 seconds (315.01 MiB/s). All three outputs were byte-identical and passed upstream verification with zero warnings and zero errors. The implementation is designed for bounded memory and streaming, but 50–100 GiB target measurements require a suitable storage and target environment.

## PS5 build status

The expected target build remains:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

The available public SDK checkout was attempted. The build stopped while compiling the required target-side libmicrohttpd dependency because its Prospero compiler could not create target executables. A direct probe reports:

```text
fatal error: 'ctype.h' file not found
```

The target sysroot is therefore incomplete for the inherited payload and no PS5 ELF is claimed. The host/Linux application and all host conversion verification remain release-ready.

## Licensing

The project remains GPLv3-or-later. MkPFS is GPLv3-or-later, and libmicrohttpd is LGPL. Original notices and third-party attribution are preserved in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
