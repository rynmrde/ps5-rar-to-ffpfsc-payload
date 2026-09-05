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
| Final commit | `11147d9` — `Add verified PS5 ELF release build` |
| Release/tag | Existing tags preserved; final ELF artifact is published in [`v0.2.0-alpha.2`](https://github.com/rynmrde/mkpfs-ps5/releases/tag/v0.2.0-alpha.2) |
| Host PFSC/PFS behavior | Preserved and passing prior compatibility checks |
| Native folder-to-exFAT | Implemented and upstream-verified |
| Web conversion task/API/UI | Implemented and Linux smoke-tested |
| PS5 ELF | Built and validated as `mkpfs-ps5-web-file-mgr.elf`; 410,888 bytes; SHA-256 `36184f375a93ab0f8808583e1a731e7f2f4a4374c9b9e64d51babd26de5e0c9d` |

## Tests and upstream verification

The clean host matrix passed:

```sh
make clean
make test-native mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder linux
MKPFS_UPSTREAM_ROOT=/home/ubuntu/work/MkPFS make compat-upstream
```

Frontend JavaScript syntax checks passed for `assets/main.js`, `assets/lang-en.js`, and `assets/lang-zh.js`. The worker benchmark, traversal/symlink checks, atomic-output checks, and HTTP conversion smoke test also passed.

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

In the final regression on identical sparse 256 MiB fixtures, serial compression completed in **1.578249 seconds (162.21 MiB/s)**, four workers completed in **0.692410 seconds (369.72 MiB/s)**, and Auto mode completed in **0.622651 seconds (411.15 MiB/s)**. All outputs were byte-identical at 813,428 bytes with SHA-256 `087bd1a7cf1c37fed8e638ffbe2fe5d1de1c36280c28281c0da3e19deb4fa185`, and each passed upstream verification with zero warnings and zero errors. This is a host smoke benchmark, not a PS5 benchmark or a 50–100 GiB endurance result.

## PS5 ELF status

The initial failure was a packaging/state problem, not an unavailable compiler. The checked-out SDK source tree contained `include/freebsd/ctype.h` but had not been installed into its documented target prefix, so `${SDK}/target/include` and `${SDK}/target/lib` did not exist. The documented SDK install was run into an isolated prefix:

```sh
make DESTDIR=/tmp/ps5-payload-sdk-staged install
```

That generated target headers, CRT objects, linker scripts, libc, pthread, and SCE stub libraries. Target zlib 1.3.1 and libmicrohttpd were then cross-compiled into the staged target homebrew prefix. The application built successfully with:

```sh
PS5_PAYLOAD_SDK=/tmp/ps5-payload-sdk-staged make
```

The resulting stripped `web-file-mgr.elf` is 410,888 bytes, has no unresolved symbols, and declares the expected PS5 `.sprx` dependencies including `libkernel_web.sprx`, `libSceLibcInternal.sprx`, `libSceNet.sprx`, `libSceIpmi.sprx`, `libSceAppInstUtil.sprx`, and `libSceUserService.sprx`. It is packaged as `mkpfs-ps5-web-file-mgr.elf` with SHA-256 `36184f375a93ab0f8808583e1a731e7f2f4a4374c9b9e64d51babd26de5e0c9d`. A physical PS5 runtime execution test remains unverified.

## Licensing

The repository remains GPLv3-or-later. MkPFS is GPLv3-or-later and libmicrohttpd is LGPL. Original notices and third-party attribution are preserved in `LICENSE` and `THIRD_PARTY_NOTICES.md`.
