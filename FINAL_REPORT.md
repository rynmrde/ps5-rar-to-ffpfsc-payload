# MkPFS for PS5 — final verified report

## Outcome

The repository now contains the complete host-tested native pipeline:

> **folder → exFAT → four-inode PFS → PFSC → `.ffpfsc`**

The previously verified PFSC and PFS stages were preserved. The missing native exFAT serializer is now implemented with deterministic upstream-compatible layout, bounded directory buffers, 1 MiB streamed file I/O, nested directory recursion, exact up-case table bytes and checksum, title-ID-derived embedded naming, cancellation checks, temporary output, and atomic rename.

The existing Web File Manager now exposes a **Convert folder** action and `/api/convert`. The action validates the selected folder, destination, output filename, compression level, and target storage; queues a background conversion task; reports progress through the existing task API and overlay; supports cancellation; and reports the final atomic output path in task history.

## Repository status

| Item | Verified value |
|---|---|
| Repository | [https://github.com/rynmrde/mkpfs-ps5](https://github.com/rynmrde/mkpfs-ps5) |
| Starting commit | `5ba07a1` |
| Final commit | `3547745` — `Finalize native parallel conversion audit` |
| Release/tag | Existing `v0.2.0-alpha` prerelease; final audit commit is ready for `v0.2.0-alpha.1` |
| Host PFSC/PFS behavior | Preserved and passing prior compatibility checks |
| Native folder-to-exFAT | Implemented and upstream-verified |
| Web conversion task/API/UI | Implemented and Linux smoke-tested |
| PS5 ELF | Not built; target sysroot blocker documented below |

## Tests and upstream verification

The clean host matrix passed:

```sh
make test-native mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder
MKPFS_UPSTREAM_ROOT=/home/ubuntu/work/MkPFS make compat-upstream
make linux
```

The real-folder compatibility test creates `sce_sys/param.json`, `eboot.bin`, and a nested `sce_sys/subdir/readme.txt`. The generated `.ffpfsc` was accepted by the upstream MkPFS verifier with **Warnings: 0** and **Errors: 0**. Upstream tree inspection of the native raw exFAT stage showed:

```text
/
|-- sce_sys
|   |-- subdir
|   |   `-- readme.txt
|   `-- param.json
`-- eboot.bin
```

The HTTP application smoke test successfully queued `/api/convert`, reached task state `done`, created the requested `.ffpfsc`, and passed the same upstream verifier with zero warnings and zero errors. It also rejected an invalid traversal source path.

## Benchmark

Using identical sparse 256 MiB fixtures, serial compression completed in **1.596709 seconds (160.33 MiB/s)**, four workers completed in **0.668967 seconds (382.68 MiB/s)**, and Auto mode completed in **0.617278 seconds (414.72 MiB/s)**. Four workers were **2.39× faster** than serial mode, a **58.10% time reduction**; Auto was **2.59× faster**, a **61.34% time reduction**. All outputs were byte-identical at 813,428 bytes with SHA-256 `087bd1a7cf1c37fed8e638ffbe2fe5d1de1c36280c28281c0da3e19deb4fa185`, and each passed upstream verification with zero warnings and zero errors. This is a host smoke benchmark, not a PS5 benchmark or a 50–100 GiB endurance result. The implementation’s directory and file-data paths are bounded and streaming, but large-volume target measurements require suitable storage and a real target environment.

## PS5 ELF status and blocker

The available public Prospero SDK checkout was attempted with:

```sh
PS5_PAYLOAD_SDK=/home/ubuntu/work/ps5-sdk/host make
```

The build stopped while compiling the target-side libmicrohttpd dependency because the Prospero compiler could not create target executables. A direct native-source probe failed with:

```text
fatal error: 'ctype.h' file not found
```

The wrapper passes `${SDK}/target/include` and `${SDK}/target/lib`, but this checkout has no `target/` directory or target libraries. It does contain `include/freebsd/ctype.h`, but that header is outside the wrapper’s target sysroot and the checkout’s `sce_stubs` are C sources rather than linked target libraries. Therefore adding an include path alone would not produce a valid PS5 ELF. No PS5 ELF is claimed, and no PS5 runtime test is claimed. This is the only known release blocker after the host application and upstream format verification passed.

## Licensing

The repository remains GPLv3-or-later. MkPFS is GPLv3-or-later and libmicrohttpd is LGPL. Original notices and third-party attribution are preserved in `LICENSE` and `THIRD_PARTY_NOTICES.md`.
