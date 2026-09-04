# MkPFS for PS5

This repository is a dedicated integration project based on [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) and [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager). It preserves the Web File Manager's native PS5 payload, embedded HTTP server, file browser, background file tasks, launcher installation reference, and responsive web assets, and adds a portable native foundation for path validation and source-folder scanning.

## Important status

The complete MkPFS writer is **not yet ported to native C/C++** in this revision. The native conversion entry point intentionally returns `ENOTSUP` rather than producing a file with a misleading `.ffpfsc` suffix. The project therefore does **not** claim to generate compatible `.ffpfsc` files, does **not** claim to provide a production-ready converter, and does **not** claim PS5 runtime testing. This is an explicit safety boundary required by the original MkPFS format and by the requirement never to report success without real conversion and verification.

The remaining work is to port the actual MkPFS PFS/exFAT serialization, compression, checksums, optional encryption/signature paths, and verification logic from the GPL-3.0-or-later Python implementation into the native payload. The existing scanner and configuration types are deliberately small, bounded-memory, and independent of Python, shell commands, or Linux utilities.

## Current build targets

The upstream file manager supports the same SDK convention as the reference project:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

The Linux-side target builds the payload's file-management code and the portable native foundation for local testing:

```sh
make linux
make test-native
```

A PS5 ELF cannot be built or tested in this environment because `PS5_PAYLOAD_SDK` and the Prospero toolchain are not installed. No claim of PS5 execution is made.

## Preserved file-manager features

The inherited payload provides browsing and sorting, copy, move, delete, rename, folder creation, text editing, multi-selection, upload and download, background task progress, cancellation, responsive English/Chinese UI, embedded assets, startup notifications, and the reference Home Screen launcher flow. The existing payload listens on port `8888` by default and attempts the next available port when needed. The PS5 ELF is normally delivered through an ELF loader listening on port `9021`; the actual HTTP port is shown by the startup notification.

## Planned native conversion architecture

The conversion job will scan and validate the source, check destination capacity, write only to a temporary output, stream file data with bounded buffers, report progress through the existing thread-safe task model, run MkPFS verification and structure verification, atomically rename the final image, and remove temporary data on cancellation or failure. The final implementation must be wired to conversion API routes only after those steps are implemented and tested against MkPFS fixtures.

## Attribution and licensing

The project is distributed under GPLv3-or-later. The original Web File Manager notices and third-party attribution are preserved. MkPFS is GPLv3-or-later. libmicrohttpd is LGPL; redistributions must satisfy its applicable terms. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
