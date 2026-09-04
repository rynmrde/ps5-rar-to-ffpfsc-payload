# MkPFS for PS5 — final verified report

## Outcome

A dedicated private GitHub repository was created and published from the referenced PS5 Web File Manager base. The repository preserves the upstream GPLv3-or-later notices and third-party attribution, adds a native C path-validation and bounded-memory folder-scanning foundation, adds portable regression tests, and adds Linux CI scaffolding.

The requested complete native MkPFS conversion was **not completed**. The current native conversion entry point returns `ENOTSUP` after preflight scanning and intentionally never creates a fake `.ffpfsc` file. Therefore this revision must not be described as production-ready, compatible with `.ffpfsc`, or PS5-tested.

## Repository and release

| Item | Verified value |
|---|---|
| Repository | [https://github.com/rynmrde/mkpfs-ps5](https://github.com/rynmrde/mkpfs-ps5) |
| Branch | `main` |
| Latest commit | `aea890f1d497e46aee1b5a297c897d86c1c70b81` — `test: validate native scanner and Linux build` |
| Release/tag | [v0.1.0-alpha](https://github.com/rynmrde/mkpfs-ps5/releases/tag/v0.1.0-alpha) |
| PS5 ELF | Not produced; `PS5_PAYLOAD_SDK` and the Prospero toolchain were unavailable |
| `.ffpfsc` output | Not produced; the native writer is intentionally guarded with `ENOTSUP` |

## Build and test results

The portable regression suite passed with:

```sh
make test-native
```

The inherited Linux payload built successfully with:

```sh
sudo apt-get install -y build-essential libmicrohttpd-dev
make linux
```

A local HTTP smoke test started `web-file-mgr-linux` and successfully retrieved the embedded UI from `http://127.0.0.1:8888/`. The response was served with the payload's existing compressed HTTP behavior.

The PS5 target was not built because the required external SDK was not present. The expected command, once the user supplies the SDK, is:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

## What is present

The inherited payload includes the file browser, browsing and sorting, copy, move, delete, rename, folder creation, text editing, multi-selection, upload/download, background task progress, cancellation, responsive web UI, startup notifications, embedded assets, and the reference Home Screen launcher flow. The new native source adds path normalization that rejects relative paths, `..` traversal, malformed components, and symlinks during recursive scans, plus bounded-memory size/file/folder accounting.

## Known limitation and required next implementation

The remaining major task is a real native port of MkPFS's PFS/exFAT serialization and `.ffpfsc` container logic. This includes the actual on-disk inode and block layout, compression framing and checksums, supported target modes, inode widths, block sizing, verification, structure verification, optional encryption/signature behavior, temporary-output finalization, and fixture-based compatibility testing. The current repository does not expose conversion routes or claim that these format-writing operations exist.

## PS5 installation and launch procedure

There is no ELF to install from this revision. After the native writer is completed and the Prospero build succeeds, the inherited reference procedure is to launch an ELF loader on the PS5, send the ELF to the loader's usual port `9021`, wait for the startup notification, and open the displayed HTTP URL from the PS5 browser or another browser on the same network. The inherited payload defaults to port `8888` and tries subsequent ports if the port is occupied. Its reference startup flow installs a Home Screen shortcut when supported and when a matching launcher is missing.

## Licensing and attribution

The project remains GPLv3-or-later. The MkPFS reference is GPL-3.0-or-later. The Web File Manager README credits the related PS5 payload projects and records the LGPL status of libmicrohttpd; those notices were preserved and expanded in `THIRD_PARTY_NOTICES.md`. Any future native port must retain the MkPFS copyright/license notices for code actually ported and must comply with the LGPL obligations for libmicrohttpd.
