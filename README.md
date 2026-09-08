# RAR to FFPFSC PS5 Payload

**RAR to FFPFSC PS5 Payload** is a userland payload for jailbroken PlayStation 5 consoles. It combines a PS5 web file manager with native RAR/7z extraction, folder-to-`.ffpfsc` conversion compatible with MkPFS, and direct URL downloads. The browser interface is designed for phones, desktop browsers, and the PS5 browser with a controller.

> **Important:** The product name does **not** mean that a RAR file can be converted directly to `.ffpfsc` in one step. The supported workflow is: **extract the RAR or 7z archive to a folder, inspect the extracted files, and then convert that folder to `.ffpfsc`.**

> **Safety notice:** This payload performs userland file operations only. It does not add kernel patches, raw-device access, or a guarantee against PS5 system errors, including CE-108262-9. Begin with non-critical files and a small test folder. Keep backups of important data.

## Contents

- [What it does](#what-it-does)
- [Requirements](#requirements)
- [Installation and first launch](#installation-and-first-launch)
- [Home Screen launcher](#home-screen-launcher)
- [PS5 web UI and controller controls](#ps5-web-ui-and-controller-controls)
- [File manager](#file-manager)
- [Convert a folder to `.ffpfsc`](#convert-a-folder-to-ffpfsc)
- [RAR/7z extraction and the RAR-to-FFPFSC workflow](#rar7z-extraction-and-the-rar-to-ffpfsc-workflow)
- [URL downloader](#url-downloader)
- [Settings, jobs, progress, and cancellation](#settings-jobs-progress-and-cancellation)
- [FAQ and troubleshooting](#faq-and-troubleshooting)
- [Features](#features)
- [Limitations](#limitations)
- [Building and testing](#building-and-testing)
- [Credits and thanks](#credits-and-thanks)
- [Roadmap](#roadmap)
- [Support and donations](#support-and-donations)

## What it does

| Capability | Description |
| --- | --- |
| **File manager** | Browses PS5, internal-storage, USB, and extended-storage locations that are mounted and exposed to the payload. It supports creating folders, copying, moving, renaming, deleting, uploading from a remote browser, downloading to a remote browser, editing small text files, and changing permissions where the filesystem permits it. |
| **Folder conversion** | Converts one selected folder into an upstream-compatible `.ffpfsc` image. The conversion path uses streamed I/O, bounded compression queues, deterministic output, cancellation checks, temporary output, and atomic publishing. |
| **RAR and 7z extraction** | Extracts supported `.rar`, legacy `.rNN`, `.7z`, and first `.7z.001` volumes with password and supported multipart handling through the embedded archive engine. |
| **URL downloader** | Downloads a direct HTTP or HTTPS URL into a selected PS5 folder as a background job. It uses bounded streaming I/O, temporary output, cancellation cleanup, and overwrite protection. |
| **Jobs panel** | Displays queued, running, completed, failed, and canceled operations, with progress, elapsed time, transfer speed, and estimated remaining time whenever the underlying operation can provide them. |

## Requirements

For normal use, you need a jailbroken PS5, an ELF payload loader compatible with the PS5 Payload SDK, a local network connection for browser control, and enough free space for the source data, temporary workspace, and final output.

For a source build, you need a POSIX C/C++ toolchain, Python 3, `zlib`, `libmicrohttpd`, and a PS5 Payload SDK installation whose target prefix includes `zlib` and `libmicrohttpd`.[5] Host archive regression tests additionally use `7z`; real RAR fixture coverage uses `rar` and `unrar` when available. These host tools are not used by the PS5 payload.

## Installation and first launch

1. Download `rar-to-ffpfsc-ps5-payload.elf` from the [latest release][7].
2. Copy the ELF to the PS5 payload manager or loader that you normally use.
3. Run the ELF and wait for the startup notification.
4. From a phone or computer on the same local network, open:

   ```text
   http://PS5-IP:6777/
   ```

   Replace `PS5-IP` with the console's local IP address.

5. On the first run, browse files only. Confirm that the expected mounted locations are readable before starting a conversion, extraction, or download.
6. Perform one small, non-critical test operation. Check the completed output before working with larger folders or archives.

The default listening port is **6777**. A valid `WFM_PORT` environment value can explicitly select another port. If `WFM_PORT` is unset, empty, invalid, or `0`, the payload safely uses port 6777. The startup notification and managed launcher use the actual successfully bound port.

If port 6777 is already occupied, the payload does not silently choose an unknown alternative. Free the port or configure a deliberate valid `WFM_PORT` override before loading the payload.

## Home Screen launcher

After the HTTP server starts successfully, the payload attempts to install or refresh only its managed Home Screen launcher entry, identified by `FMGR88888`. This is a protected **PS5 title ID**, not a network port.

By default, the managed launcher opens `http://127.0.0.1:6777/`. When a valid `WFM_PORT` override is used, the launcher is generated with the actual bound port instead. The launcher is never installed before server readiness. Existing unrelated application metadata is not overwritten. A launcher-installation failure does not stop the file manager; use the network URL reported by the startup notification instead.

Launcher visibility and PS5 installation permissions are target-hardware behaviors. They must be confirmed on a physical PS5.

## PS5 web UI and controller controls

The web UI exposes browsing, source selection, destination selection, `.ffpfsc` conversion, RAR/7z extraction, direct URL downloads, settings prompts, output management, errors, and job history. The same interface is available to a phone or desktop browser.

| Control | Action |
| --- | --- |
| **D-pad** | Move focus through file rows and focusable controls. |
| **Cross / Enter** | Activate the currently focused control or open the focused folder. |
| **Backspace / Alt+Up** | Open the parent folder. |
| **F5** | Refresh the current folder. |
| **Refresh** | Reload the current folder from the API. |
| **Jobs / Cancel** | Review task status and cooperatively cancel the active task. |

The **Get URL** action is available in the PS5 browser. Browser-to-PS5 upload and PS5-to-browser download actions are deliberately limited to remote browsers, because the internal PS5 browser does not provide a reliable local file-picker or download destination workflow.

## File manager

The runtime follows the upstream PS5 Web File Manager design for root discovery, directory enumeration, launcher flow, and standard file APIs.[2] The UI uses the available roots response and may show readable mounted locations such as `/data`, `/mnt/usb0`, or `/mnt/usb1` when they actually exist on the console.

Do not assume that every path exists or is readable. Refresh the file view and open only mounted locations that are presented by the UI or confirmed by the API. The file API validates paths, rejects traversal components and unsafe encoded paths, and preserves containment and symlink protections. The project does not use kernel access or raw-device access as a filesystem workaround.

## Convert a folder to `.ffpfsc`

1. Browse to the source folder and select exactly one folder.
2. Browse to the existing folder where the `.ffpfsc` file should be created.
3. Select **Convert folder**.
4. Enter an output filename. The UI adds `.ffpfsc` if it is missing.
5. Select a compression level from 0 to 9. The default is 7.
6. Start the job and monitor it in the Jobs panel.
7. Wait for **Done** before using the output file.

The converter scans and validates the source before creating an output file. It writes to a same-directory temporary file, uses streamed I/O and bounded memory, checks cancellation during work, synchronizes completed data, and atomically publishes the final file only after success. Existing output names are rejected rather than overwritten.

The serializer passes the upstream MkPFS verifier on the project fixtures.[1] Serial and parallel conversion produced byte-identical output for the same test input and settings. The reproducible 4,000-file, 64-byte host benchmark measured a mean serial conversion time of **1.270 seconds before** and **1.146 seconds after** the small-file optimization, a **9.76% improvement**. This is a host benchmark, not a PS5 storage-performance claim.

Finalization writes the PFSC stream directly inside the same-directory atomic PFS output and then performs the required full PFSC decompression/offset verification in place. It does not use a second full-size PFSC temporary file or a second full-file copy. On the reproducible 32,000-file compressible workload, median finalization time changed from **2.173 seconds to 1.973 seconds** (**9.22% faster**). On a 4,096-file, 64 KiB incompressible workload, where the physical final-output I/O is substantial, it changed from **0.300 seconds to 0.077 seconds** (**74.34% faster**). Each comparison used the recorded pre-optimization source, identical input and output name, and a byte-identity check; both optimized outputs passed the upstream verifier with zero warnings and zero errors. These are host measurements, not PS5 storage-performance claims.

## RAR/7z extraction and the RAR-to-FFPFSC workflow

### Supported archive workflow

1. Browse to the archive and select one supported file: `.rar`, legacy `.rNN`, `.7z`, or the first `.7z.001` volume.
2. Select **Extract archive**.
3. Enter an existing destination folder and a new output-folder name.
4. Enter the archive password when required, or leave it blank for an unprotected archive.
5. Choose extraction workers: `auto` or an integer from 1 through 8.
6. Monitor the extraction job until its state is **Done**.
7. Browse into the extracted folder and inspect its contents.
8. Select that extracted folder and follow the [folder conversion steps](#convert-a-folder-to-ffpfsc) to create `.ffpfsc`.

The embedded extraction engine is adapted from `bizkut/unrar-ps5`, including its bundled 7-Zip decoder.[3] Multipart support follows the archive naming patterns supported by that upstream engine. The project regression suite covers single-volume, password-protected, and multipart RAR/7z fixtures.

For safety, extraction occurs inside a private `.mkpfs-extract-<task-id>.tmp` directory under the selected destination. The final output folder is published only after successful decoding. On failure or cancellation, the private staging directory is removed without following archive-created symlinks. RAR symbolic-link extraction is disabled by the embedded adapter.

## URL downloader

1. Browse to the existing folder where the downloaded file should be saved.
2. Select **Get URL**.
3. Paste a direct `http://` or `https://` URL.
4. Confirm the destination folder and output filename.
5. Start the job and monitor its status in the Jobs panel.

URL downloads use a fixed 64 KiB streaming buffer. When the server sends a content length, the Jobs panel can display progress, speed, and ETA. Streams without a known length still report transferred bytes and speed where available.

The output filename must be a safe single filename. Existing destination files are rejected. Each download writes to a private same-directory temporary file and is published only after successful completion. On filesystems that support it, publication uses a no-replace hard-link step. On FAT/exFAT, the payload performs a second existence check and a same-directory rename. Failure, cancellation, invalid responses, and write errors remove the temporary file.

The PS5 build uses its SceHttp client for direct HTTP and HTTPS GET requests. HTTPS certificate verification is not disabled. Direct authenticated sources, cookies, custom headers, request bodies, redirects, checksum manifests, resume, and concurrent transfers are not supported. Use a final direct URL from a source you trust. To retry a failed download, correct the URL or destination and create a new job.

## Settings, jobs, progress, and cancellation

Compression and archive worker settings accept `auto` or an integer from 1 through 8. The system permits one active filesystem job at a time. This makes disk usage, output ownership, destination validation, and cancellation behavior predictable.

| Job state | Meaning |
| --- | --- |
| **Queued** | The job was accepted and is waiting to begin. |
| **Preparing / Checking** | The source, destination, space, or operation prerequisites are being validated. |
| **Running** | Data is being converted, extracted, copied, moved, or downloaded. |
| **Finishing** | Output synchronization and final publication are in progress. |
| **Done** | The final output was published successfully. |
| **Failed** | The job ended with an error; inspect the displayed message. |
| **Canceled** | The cancellation request was observed and temporary task output was cleaned up. |

Cancellation is cooperative. Conversion checks between streamed work units, extraction checks upstream callbacks, and downloads check between bounded response reads. A slow remote server can delay a download cancellation until the active receive timeout completes. Wait for a terminal **Canceled** or **Failed** state before reusing an output name.

## FAQ and troubleshooting

| Problem | What to check |
| --- | --- |
| The web page does not open | Confirm the PS5 IP address, use `http://PS5-IP:6777/`, and confirm that the startup notification reported a running server. If `WFM_PORT` was explicitly configured, use that configured/bound port. |
| No startup notification appears | Confirm that the new ELF was selected in the payload manager and that port 6777 is not occupied. Check payload-manager logs if available. |
| The Home Screen launcher does not appear | The web interface can still be used through the network URL. Confirm server readiness first. Launcher installation permissions and visibility require PS5 hardware verification. |
| No files or folders are displayed | Refresh the UI and inspect the readable roots. Browse only mounted paths that actually exist, such as `/data` or `/mnt/usb0` when present. Do not attempt to bypass permissions with kernel patches. |
| Conversion or extraction is rejected | Confirm the source type, destination existence, write access, free space, safe output name, and absence of a file/folder with the same output name. |
| A URL download fails | Use a final direct URL without sign-in or redirects, verify PS5 network access and free space, then submit a new job. Do not disable HTTPS certificate checks to work around a TLS failure. |
| Cancellation does not finish immediately | Cancellation is cooperative. Wait for the current read or work unit to finish and for the job to enter a terminal state. |
| A `.ffpfsc` file is not usable | Start with a small source folder, confirm the job completed, verify the output size, and test against the expected MkPFS workflow. The upstream verifier is available for host-side validation. |

## Features

- Default port **6777**, synchronized with the startup notification and managed Home Screen launcher.
- PS5 web file browser with explicit source and destination workflows.
- Bounded-memory, deterministic folder-to-`.ffpfsc` conversion.
- RAR/7z extraction with password input, supported multipart archives, staging, progress, and cooperative cancellation.
- Direct HTTP/HTTPS URL downloads with background jobs, bounded streaming, cleanup, and overwrite protection.
- Job status, history, progress, speed, ETA, errors, and cancellation.
- Path validation, traversal rejection, symlink safeguards, no-follow cleanup, collision checks, and same-directory atomic finalization.
- English and Chinese UI strings with D-pad-friendly focus navigation.

## Limitations

The following must be verified on a physical PS5: Home Screen launcher appearance, filesystem permissions and mounts, SceHttp/TLS compatibility, internal-browser behavior, and long-running storage stability. This project cannot guarantee the absence of CE-108262-9 or any other PS5 error.

Downloads are intentionally limited to direct HTTP/HTTPS GET requests. They do not support authentication, cookies, custom headers, redirects, resume, checksums, or concurrent transfers. Archive extraction is limited to formats and multipart patterns implemented by the bundled upstream decoder; arbitrary archive formats are not supported.

Always begin with browsing, then a small non-critical extraction, conversion, or download. Verify the completed output before using large folders or important files.

## Building and testing

```sh
# Host build and core functional tests
make linux test-native test-archive test-url-download test-default-port
./tools/smoke_http.sh
./tools/test_archive_http.sh

# Verify generated FFPFSC output with the upstream MkPFS verifier
make compat-upstream MKPFS_UPSTREAM_ROOT=/path/to/MkPFS

# Reproducible 4,000-small-file benchmark
./tools/benchmark_small_files.sh

# Reproducible 32,000-small-file finalization benchmark. It compiles the
# recorded pre-optimization source, checks byte identity, and prints timings.
make benchmark-finalization

# Host-side memory, HTTP, and filesystem safety checks
./audit-memory-safety.sh
python3 audit-filesystem-api-safety.py
python3 audit-http-robustness.py ./web-file-mgr-linux /tmp/http.log

# PS5 target build after staging the SDK and target dependencies
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

The PS5 target output is `rar-to-ffpfsc-ps5-payload.elf`.

## Credits and thanks

This project is built on real upstream open-source work. Thank you to the maintainers and contributors of the following projects:

| Project | Contribution to this project |
| --- | --- |
| [PSBrew/MkPFS][1] | Reference implementation and verifier for `.ffpfsc` format compatibility. |
| [owendswang/ps5-web-file-manager][2] | PS5 runtime architecture, file manager, root discovery, launcher flow, and file-management APIs. |
| [bizkut/unrar-ps5][3] | Native RAR/7z extraction engine and archive integration basis. |
| [7-Zip][4] | Decoder included with the vendored 7z extraction sources. |
| [PS5 Payload SDK][5] | PS5 ELF build tooling and platform API integration. |
| [GNU libmicrohttpd][6] | Embedded HTTP server. |

Required licenses, notices, and attribution are retained in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Roadmap

The developer's next project, **Windows on PS5**, is already prepared and will be publicly released when these support goals are reached: **$30 total support = release; $45 total support = completion and release within 10 days.** This is the developer's stated release commitment.

## Support and donations

| Network | Address |
| --- | --- |
| TON | `UQD80q4Pm-9cYzMmfB8rbgRrJAqRxuAmrbGm4GqFEgtdFSLL` |
| Polygon (POL) | `0x0dE5511076bc70F489B1139485BbC73cd96cdc04` |
| Solana | `A4FWhkKrUgEW3vZvsT19Koh5tWVCVK1LGw1Xwf9LQRLF` |
| TRON | `TAhUU9RiB5VrUZ4z3cHYvnsGampig5aaEm` |

If you would like to donate using another cryptocurrency that is not listed here, please contact us through the project's issue/contact section so we can add it. If you encounter any problem with a donation address or transaction, please report it there as well.

## References

[1]: https://github.com/PSBrew/MkPFS "PSBrew MkPFS"
[2]: https://github.com/owendswang/ps5-web-file-manager "owendswang PS5 Web File Manager"
[3]: https://github.com/bizkut/unrar-ps5 "bizkut unrar-ps5"
[4]: https://www.7-zip.org/ "7-Zip"
[5]: https://github.com/ps5-payload-dev/sdk "PS5 Payload SDK"
[6]: https://www.gnu.org/software/libmicrohttpd/ "GNU libmicrohttpd"
[7]: https://github.com/rynmrde/mkpfs-ps5/releases/latest "RAR to FFPFSC PS5 Payload releases"
