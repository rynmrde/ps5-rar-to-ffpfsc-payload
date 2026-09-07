# MkPFS PS5

**MkPFS PS5** is a native PS5 payload that combines the upstream Web File Manager with safe folder-to-`.ffpfsc` conversion, RAR/7z extraction, and direct URL downloading. It is designed for a jailbroken PS5 running a compatible ELF payload loader. The browser UI works on a phone, desktop browser, and the PS5 browser with a controller.

> This project performs userland file operations only. It does not make kernel patches, access raw block devices, or provide a guarantee against console crashes. Test first with non-critical files and keep a current backup.

## What it does

| Capability | Description |
| --- | --- |
| **File Manager** | Browses available PS5, internal, USB, and extended-storage paths exposed to the payload. It can copy, move, rename, delete, upload, download, create folders, edit small text files, and change permissions where the filesystem supports them. |
| **MkPFS conversion** | Converts one selected folder into an upstream-compatible `.ffpfsc` image using streamed I/O, bounded compression queues, deterministic output, cancellation checks, and temporary-output publishing. |
| **RAR/7z extraction** | Extracts `.rar`, legacy `.rNN`, `.7z`, and first `.7z.001` archives through the vendored `unrar-ps5` engine. Passwords and supported multipart conventions are handled by that engine. |
| **URL downloader** | Saves a direct HTTP or HTTPS URL to a chosen PS5 folder as a background job. The PS5 build uses the system SceHttp client; HTTPS certificate verification is not disabled. |
| **Jobs** | Shows queued, running, finished, failed, and canceled file operations with available progress, speed, elapsed time, and ETA information. |

## Requirements

You need a jailbroken PS5, an ELF payload loader compatible with the [PS5 Payload SDK][sdk], network access for browser control, and sufficient free storage for every requested operation. Build hosts need a POSIX C/C++ toolchain, `zlib`, `libmicrohttpd`, Python 3, and the SDK prefix containing target `zlib` and `libmicrohttpd`.

RAR fixture tests additionally use `7z`; real RAR fixture coverage uses `rar` and `unrar` when those tools are available. These are test-only host dependencies and are not used by the PS5 payload.

## Installation

1. Download `mkpfs-ps5-web-file-mgr.elf` from the latest GitHub release.
2. Transfer it to the payload manager or loader used by the console.
3. Load the ELF. The payload starts its local web server before attempting the managed Home Screen launcher refresh.
4. Read the startup notification for the **actual bound port**. From another device on the same network, open `http://PS5-IP:PORT/`.

To request a particular port, set `WFM_PORT` in the payload environment before launch. When it is not set, the server asks the operating system for an available port. The startup notification and the managed launcher both use the actual bound port, not a hard-coded `8888` value.

## First Launch and Home Screen launcher

After successful server initialization, MkPFS PS5 refreshes only its managed `FMGR88888` launcher entry. The launcher metadata points to `http://127.0.0.1:ACTUAL_PORT/` and is not installed before the server is ready. Existing unrelated application metadata is never overwritten. If launcher refresh fails, the file manager remains available through the reported network URL.

The launcher and real storage access are PS5-runtime behaviors. They cannot be fully verified from a Linux host build. If the launcher does not appear, first confirm that the payload notification reported a running server and open the reported URL from another device.

## PS5 UI and controller use

The interface keeps ordinary focusable buttons and rows for controller, touch, mouse, and keyboard input. Use the D-pad to move through file rows, **Cross/Enter** to activate the focused control, **Backspace** or **Alt+Up** for the parent folder, and **F5** to refresh. The **Get URL** action is intentionally available in the PS5 browser; browser-to-device upload and browser-download controls remain hidden there because they require a remote browser file picker or download target.

For each task, select a source in the browser when required, navigate to or enter the destination, provide the requested name or settings, then watch the task panel. The current destination remains usable after a failed or canceled operation because partial task staging is removed.

## File Manager

The payload follows the upstream PS5 Web File Manager runtime for root discovery, directory enumeration, the launcher flow, and standard file APIs. The `/api/roots` diagnostic response reports readable locations when the root listing is empty. File APIs reject traversal components, unsafe encoded paths, and unsupported symlink operations where applicable. The project does not add raw-device or kernel access as a storage workaround.

## Convert a folder to `.ffpfsc`

Select exactly one folder, browse to the destination directory, choose **Convert folder**, enter an output name and compression level, and start the job. The conversion pipeline scans and validates the input before creating a same-destination temporary output. It then writes an exFAT image and PFSC wrapper with bounded streaming memory before atomically publishing the final file. Existing output names are rejected, rather than overwritten.

The serializer is compatible with the upstream MkPFS verifier on the project fixtures. It supports Auto or explicitly bounded parallel compression workers. Serial and parallel operation produce byte-identical output for the same input and settings.

### Small-file performance

The reproducible `tools/benchmark_small_files.sh` workload contains 4,000 deterministic 64-byte files. The final serial host benchmark measured an average of **1.270 seconds before** and **1.146 seconds after** the small-file changes, a **9.76% improvement**. The outputs compared byte-for-byte. This is a host result, not a PS5 storage-performance claim.

## RAR and 7z extraction

Select one supported archive, choose **Extract archive**, provide an explicit destination folder and output folder name, then supply a password or worker setting if needed. Extraction runs in a private `.mkpfs-extract-<task-id>.tmp` directory under the selected destination. A completed output directory is published only after the decoder succeeds. Cancellation or failure recursively removes that private directory without following archive-created symlinks.

The native extraction engine is based on [`bizkut/unrar-ps5`][unrar-ps5], including its bundled 7-Zip decoder. The UI accepts `.rar`, legacy `.rNN`, `.7z`, and first `.7z.001` volumes. Multipart support follows the upstream decoder’s supported naming conventions. RAR symbolic-link extraction is disabled in the embedded adapter.

## Downloader

Choose **Get URL**, paste a direct `http://` or `https://` URL, choose a destination folder, and confirm a filename. Downloads are queued as native background jobs and write with a fixed 64 KiB buffer. When a response provides a content length, the Jobs panel displays progress, speed, and ETA. Streaming responses still show transferred bytes and speed when available.

The destination filename must be a safe single filename; existing files are rejected. The task creates a private same-directory temporary file, checks cancellation during reads, synchronizes completed data, and publishes only after success. On filesystems supporting hard links, publication uses a no-replace hard-link step. On FAT/exFAT, which do not support hard links, the implementation performs a second existence check followed by a same-directory rename. Cancellation, network failure, HTTP failure, and write failure remove the temporary file.

The PS5 build uses SceHttp for direct HTTP and HTTPS GET requests with certificate verification enabled for HTTPS. It does not send credentials, custom headers, cookies, or request bodies, and it intentionally does not follow redirects. Submit the final direct URL supplied by a trusted source. Failed downloads do not overwrite a destination; correct the URL or destination and submit a new job to retry.

## Settings, jobs, progress, and cancellation

Compression and archive worker values accept **Auto** or an integer from 1 to 8. The task view tracks one active filesystem job at a time to keep destination validation, disk use, and cancellation behavior predictable. Use the visible cancel control for a running task. Cancellation is cooperative: conversion checks between streamed work units, extraction checks upstream callbacks, and downloading checks between bounded response reads. A slow remote peer can delay downloader cancellation until its configured receive timeout returns.

## FAQ and troubleshooting

| Symptom | Check |
| --- | --- |
| No notification or browser page | Confirm the payload manager loaded the new ELF, then check its logs and whether the selected port is already occupied. Use the actual port printed by the startup notification. |
| The Home Screen launcher is missing | The server can still be used from another device. Confirm the payload reached server readiness; launcher installation depends on the console’s runtime install permissions and needs hardware verification. |
| No folders are shown | Refresh, inspect the readable roots reported by the UI/API, and browse a mounted path such as `/data` or `/mnt/usb0` only when it is actually present. Do not work around permissions with kernel patches. |
| A conversion or extraction is rejected | Check that the source type is correct, the destination exists and is writable, the output name is unused, and sufficient space is available. |
| A download fails | Use a direct URL without authentication or redirects, check console networking and free space, and retry as a new job. HTTPS certificate or TLS compatibility failures must not be bypassed. |
| Cancellation takes time | It is cooperative by design. Wait for the task to report **Canceled** and verify that the temporary task output has been removed before reusing the name. |

## Limitations

PS5 hardware behavior, SceHttp TLS compatibility, filesystem permissions, launcher visibility, and long-running storage stability must be validated on the target console. The project has no hardware guarantee against CE-108262-9 or any other system error. Start with browsing only, then a tiny non-critical conversion or download, confirm the result, and only then use larger folders.

Downloads are direct GET requests only. They do not support authenticated sources, cookies, custom headers, redirects, checksum manifests, resume, or concurrent transfers. Archive extraction supports only the formats and multipart patterns implemented by the bundled upstream decoder. This project does not support arbitrary archive formats.

## Features

- PS5-native web file browser with explicit source and destination workflows.
- Actual-port startup notification and managed `FMGR88888` Home Screen launcher refresh.
- Bounded-memory MkPFS conversion with deterministic serial or parallel PFSC output.
- Native RAR and 7z extraction with password, multipart, progress, staging, and cooperative cancellation support.
- Direct HTTP/HTTPS URL downloader with background jobs, bounded streaming I/O, temporary output, and no overwrite by default.
- Traversal checks, no-follow cleanup, symlink safeguards, collision checks, and atomic/same-directory finalization.
- English and Chinese UI strings with controller-focused navigation.

## Building and testing

```sh
# Host build and core test coverage
make linux test-native test-archive mkpfs-pfsc
./tools/smoke_http.sh
./tools/test_archive_http.sh
make test-url-download

# Upstream MkPFS format verification
make compat-upstream MKPFS_UPSTREAM_ROOT=/path/to/MkPFS

# Reproducible small-file benchmark
./tools/benchmark_small_files.sh

# Available memory and HTTP robustness checks
./audit-memory-safety.sh
python3 audit-filesystem-api-safety.py
python3 audit-http-robustness.py

# PS5 target build after staging the SDK and target dependencies
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

Run `make test-archive` with host archive tools installed for real single-volume, password-protected, and multipart RAR/7z fixtures. The generated PS5 ELF is `web-file-mgr.elf`.

## Credits and thanks

This project is built on real upstream work. MkPFS conversion compatibility follows [PSBrew/MkPFS][mkpfs]. The PS5 runtime architecture, web file manager behavior, storage roots, launcher flow, and file-management design originate from [owendswang/ps5-web-file-manager][web-file-manager]. RAR and 7z extraction is adapted from [bizkut/unrar-ps5][unrar-ps5] and its bundled [7-Zip][seven-zip] decoder. The HTTP server uses [GNU libmicrohttpd][microhttpd]. The payload build uses the [PS5 Payload SDK][sdk].

See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the project license, upstream licenses, notices, and attribution.

## Roadmap

The developer’s next project, **Windows on PS5**, is already prepared and will be publicly released when support reaches these goals: **$30 total support = release; $45 total support = completion and release within 10 days.** This is the developer’s stated release commitment.

## Support and donations

| Network | Address |
| --- | --- |
| TON | `UQD80q4Pm-9cYzMmfB8rbgRrJAqRxuAmrbGm4GqFEgtdFSLL` |
| Polygon (POL) | `0x0dE5511076bc70F489B1139485BbC73cd96cdc04` |
| Solana | `A4FWhkKrUgEW3vZvsT19Koh5tWVCVK1LGw1Xwf9LQRLF` |
| TRON | `TAhUU9RiB5VrUZ4z3cHYvnsGampig5aaEm` |

If you would like to donate using another cryptocurrency that is not listed here, please contact us through the project’s issue/contact section so we can add it. If you encounter any problem with a donation address or transaction, please report it there as well.

[mkpfs]: https://github.com/PSBrew/MkPFS "PSBrew MkPFS"
[web-file-manager]: https://github.com/owendswang/ps5-web-file-manager "owendswang PS5 Web File Manager"
[unrar-ps5]: https://github.com/bizkut/unrar-ps5 "bizkut unrar-ps5"
[seven-zip]: https://www.7-zip.org/ "7-Zip"
[microhttpd]: https://www.gnu.org/software/libmicrohttpd/ "GNU libmicrohttpd"
[sdk]: https://github.com/ps5-payload-dev/sdk "PS5 Payload SDK"
