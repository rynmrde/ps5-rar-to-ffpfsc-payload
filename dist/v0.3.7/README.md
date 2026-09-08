# RAR to FFPFSC PS5 Payload

RAR to FFPFSC PS5 Payload is a userland payload for jailbroken PlayStation 5 consoles. It combines a PS5 web file manager with native RAR/7z extraction, folder-to-`.ffpfsc` conversion compatible with MkPFS, and direct URL downloads. The browser interface is designed for phones, desktop browsers, and the PS5 browser with a controller.

> **Important:** The product name does not mean that a RAR file can be converted directly to `.ffpfsc` in one step. The supported workflow is: extract the RAR or 7z archive to a folder, inspect the extracted files, and then convert that folder to `.ffpfsc`.


## Roadmap

My next project, **Windows on PS5**, is already prepared. I will release it publicly when total support reaches **$30**. If total support reaches **$45**, I will complete it and release it within 10 days. This is my stated release commitment, not just an idea for the future.

## Support and donations

If this project is useful to you and you would like to support my work, these are the addresses I use:

| Network | Address |
| --- | --- |
| TON | `UQD80q4Pm-9cYzMmfB8rbgRrJAqRxuAmrbGm4GqFEgtdFSLL` |
| Polygon (POL) | `0x0dE5511076bc70F489B1139485BbC73cd96cdc04` |
| Solana | `A4FWhkKrUgEW3vZvsT19Koh5tWVCVK1LGw1Xwf9LQRLF` |
| TRON | `TAhUU9RiB5VrUZ4z3cHYvnsGampig5aaEm` |

If you would like to donate using another cryptocurrency that is not listed here, please contact me through the project's issue/contact section so I can add it. If you encounter any problem with a donation address or transaction, please report it there as well.

## What it does

I built this payload to keep the everyday PS5 file workflow in one place: browse storage, extract an archive, convert a prepared folder to `.ffpfsc`, or download a direct file URL to the console.

| Capability | What it does |
| --- | --- |
| **File manager** | Lets you browse mounted PS5, internal-storage, USB, and extended-storage locations exposed to the payload. You can create folders, copy, move, rename, delete, upload from a remote browser, download to a remote browser, edit small text files, and change permissions where the filesystem allows it. |
| **Folder conversion** | Converts one selected folder into an upstream-compatible `.ffpfsc` image. I kept the conversion path streamed, bounded in memory, deterministic, cancelable, and atomically published. |
| **RAR and 7z extraction** | Extracts supported `.rar`, legacy `.rNN`, `.7z`, and first `.7z.001` volumes, with passwords and the multipart patterns supported by the embedded archive engine. |
| **URL downloader** | Downloads a direct HTTP or HTTPS URL into a selected PS5 folder as a background job, using bounded streaming I/O, temporary output, cancellation cleanup, and overwrite protection. |
| **Jobs panel** | Shows queued, running, completed, failed, and canceled work. It includes progress, elapsed time, transfer speed, and estimated remaining time whenever the operation can provide them. |

## Installation and first launch

1. Download `rar-to-ffpfsc-ps5-payload.elf` from the [latest release][7].
2. Add the ELF to the PS5 payload manager or loader you normally use.
3. Run the payload and wait for the startup notification.
4. Open the following address from a phone or computer on the same local network:

   ```text
   http://PS5-IP:6777/
   ```

   Replace `PS5-IP` with your console's local IP address.

5. On the first run, browse files only. Confirm that the mounted locations you need are readable before starting a conversion, extraction, or download.

Port **6777** is the normal listening port. A valid `WFM_PORT` environment value can deliberately select a different port. If `WFM_PORT` is unset, empty, invalid, or `0`, the payload uses port 6777. The startup notification and managed launcher follow the port that actually bound successfully.

If port 6777 is already in use, the payload does not silently switch to an unknown port. Free the port or deliberately configure a valid `WFM_PORT` override before loading the payload.

## PS5 web UI and controller controls

I kept the interface usable from the PS5 browser while preserving the phone and desktop experience. You can browse, choose sources and destinations, convert folders, extract archives, download URLs, review errors, manage outputs, and follow job history from the same UI.

| Control | Action |
| --- | --- |
| **D-pad** | Moves focus through file rows and focusable controls. |
| **Cross / Enter** | Activates the focused control or opens the focused folder. |
| **Backspace / Alt+Up** | Opens the parent folder. |
| **F5** | Refreshes the current folder. |
| **Refresh** | Reloads the current folder through the API. |
| **Jobs / Cancel** | Opens task status and cooperatively cancels the active task. |

The **Get URL** action is available in the PS5 browser. Uploading from a browser to PS5 storage and downloading a PS5 file to a browser are intentionally restricted to remote browsers, because the internal PS5 browser does not provide a reliable file-picker or download-destination workflow for those actions.

## File manager

The runtime follows the root discovery, directory enumeration, launcher flow, and standard file-API design from the upstream PS5 Web File Manager.[2] The UI asks the API for available roots and may show readable mounted locations such as `/data`, `/mnt/usb0`, or `/mnt/usb1` when they really exist on the console.

Do not assume every path exists or is readable. Refresh the view and use mounted locations presented by the UI or confirmed by the API. I kept the path checks in place: traversal components and unsafe encoded paths are rejected, containment is checked, and symlink protections are preserved. This payload does not use kernel or raw-device access to work around filesystem permissions.

## Convert a folder to `.ffpfsc`

1. Browse to the source folder and select exactly one folder.
2. Browse to the existing folder where the `.ffpfsc` file should be created.
3. Select **Convert folder**.
4. Enter an output filename. The UI adds `.ffpfsc` when it is missing.
5. Select a compression level from 0 to 9. The default is 7.
6. Start the job and watch it in the Jobs panel.
7. Wait for **Done** before using the output file.

Before creating an output, the converter scans and validates the source. It checks that the destination has enough **peak working space** for the full temporary exFAT image and the concurrently-created final PFS/PFSC container; this may be close to twice the source-image size for incompressible content. It uses bounded streaming memory, checks for cancellation during work, synchronizes durable data, and publishes the final file atomically only after success. Existing output names are rejected instead of overwritten.

Each conversion writes a small, atomic recovery journal in `/data/mkpfs-resume` and a private `.<output>.mkpfs-conversion.incomplete` note beside the output. If the payload process is lost or reloaded, a fresh payload start scans the journal directory and automatically resumes one valid interrupted conversion from its last synchronized checkpoint. During exFAT creation, checkpoints are recorded only after whole source files have been written, flushed, and synchronized. On restart, the payload re-enumerates the source in the same deterministic order, validates its metadata fingerprint, and continues from the last completed file rather than rebuilding completed file data. PFSC compression and verification resume from their durable block checkpoints. The final `.ffpfsc` remains absent until validation and atomic publication complete.

After the exFAT snapshot is durable, restart recovery uses that immutable private stage and does **not** scan the original source tree again. This avoids another long metadata walk and permits the final PFSC/PFS work to finish even if that source was temporarily disconnected. During the exFAT stage itself, the source must still be available for the deterministic metadata validation pass. A source rename, resize, replacement, or modification causes recovery to stop safely and discard the unsafe checkpoint instead of mixing old staged bytes with changed files.

The private stage files can be large because they hold the required exFAT snapshot and partially-built output. They are removed after a successful job or a user-requested cancellation. If a checkpoint is invalid, the staged source is unsafe, or the source changes while its exFAT image is being rebuilt, automatic recovery stops and removes the invalid private stages rather than retrying indefinitely. Do not use or rename a private stage file as a final output. Automatic resume applies to conversions started by this recovery-enabled version; older temporary files that predate it cannot be safely reconstructed. Confirm that no old payload is still running, then remove only those older temporary files manually after checking their destination and free space.

The serializer passes the upstream MkPFS verifier on the project fixtures.[1] Serial and parallel conversion produced byte-identical output for the same test input and settings. The reproducible 4,000-file, 64-byte host benchmark measured a mean serial conversion time of **1.270 seconds before** and **1.146 seconds after** the small-file optimization, a **9.76% improvement**. This is a host benchmark, not a PS5 storage-performance claim.

I also improved the finalization path. PFSC data is written directly inside the same-directory atomic PFS output, then fully verified in place by decompressing and checking PFSC offsets. The payload no longer creates a second full-size PFSC temporary file or copies that file again into the PFS container. On the reproducible 32,000-file compressible workload, median finalization time changed from **2.173 seconds to 1.973 seconds** (**9.22% faster**). On a 4,096-file, 64 KiB incompressible workload, where final-output I/O matters more, it changed from **0.300 seconds to 0.077 seconds** (**74.34% faster**). Every comparison used the recorded pre-optimization source, identical input and output name, and a byte-identity check. Both optimized outputs passed the upstream verifier with zero warnings and zero errors. These are host measurements, not PS5 storage-performance claims.

## RAR/7z extraction and the RAR-to-FFPFSC workflow

1. Browse to the archive and select one supported file: `.rar`, legacy `.rNN`, `.7z`, or the first `.7z.001` volume.
2. Select **Extract archive**.
3. Enter an existing destination folder and a new output-folder name.
4. Enter the archive password when required, or leave it blank for an unprotected archive.
5. Choose extraction workers: `auto` or an integer from 1 through 8.
6. Monitor the extraction job until its state is **Done**.
7. Browse into the extracted folder and inspect its contents.
8. Select that extracted folder and use the [folder conversion steps](#convert-a-folder-to-ffpfsc) to create `.ffpfsc`.

The embedded extraction engine is adapted from `bizkut/unrar-ps5`, including its bundled 7-Zip decoder.[3] Multipart handling follows the naming patterns supported by that upstream engine. I added regression coverage for single-volume, password-protected, and multipart RAR/7z fixtures.

For safer output handling, extraction happens in a private `.mkpfs-extract-<task-id>.tmp` directory below the destination you choose. The final output folder is published only after decoding succeeds. If a job fails or is canceled, its private staging directory is removed without following archive-created symlinks. RAR symbolic-link extraction is disabled by the embedded adapter.

## URL downloader

1. Browse to the existing folder where you want to save the download.
2. Select **Get URL**.
3. Paste a direct `http://` or `https://` URL.
4. Confirm the destination folder and output filename.
5. Start the job and monitor it in the Jobs panel.

Downloads use a fixed 64 KiB streaming buffer. When the server provides a content length, the Jobs panel can show progress, speed, and ETA. Streams without a known length still show transferred bytes and speed where available.

The output filename must be one safe filename. Existing destination files are rejected. Each download is written to a private temporary file in the same directory and published only after it completes successfully. On filesystems that support it, publication uses a no-replace hard-link step. On FAT/exFAT, the payload checks again for an existing name before the same-directory rename. Failed downloads, cancellations, invalid responses, and write errors remove the temporary file.

The PS5 build uses SceHttp for direct HTTP and HTTPS GET requests. HTTPS certificate verification is not disabled. Authenticated sources, cookies, custom headers, request bodies, redirects, checksum manifests, resume, and concurrent transfers are not supported. Use a final direct URL from a source you trust. To retry a failed download, correct the URL or destination and start a new job.

## Settings, jobs, progress, and cancellation

Compression and archive worker settings accept `auto` or an integer from 1 through 8. Only one filesystem job runs at a time. I chose that behavior to keep disk use, output ownership, destination validation, and cancellation predictable.

| Job state | Meaning |
| --- | --- |
| **Queued** | The job was accepted and is waiting to begin. |
| **Preparing / Checking** | The source, destination, free space, or other prerequisites are being checked. |
| **Running** | Data is being converted, extracted, copied, moved, or downloaded. |
| **Finishing** | Output synchronization and final publication are in progress. |
| **Done** | The final output was published successfully. |
| **Failed** | The job ended with an error; read the displayed message. |
| **Canceled** | The cancellation request was observed and temporary task output was cleaned up. |

Cancellation is cooperative. Conversion checks between streamed work units, extraction checks the upstream callbacks, and downloads check between bounded response reads. A slow remote server can delay download cancellation until the active receive timeout ends. Wait for a terminal **Canceled** or **Failed** state before reusing an output name.

## FAQ and troubleshooting

| Problem | What to check |
| --- | --- |
| The web page does not open | Confirm the PS5 IP address, use `http://PS5-IP:6777/`, and confirm that the startup notification reported a running server. If you explicitly configured `WFM_PORT`, use the configured/bound port instead. |
| No startup notification appears | Confirm that the new ELF was selected in the payload manager and that port 6777 is not occupied. Check payload-manager logs if they are available. |
| The Home Screen launcher does not appear | You can still use the web interface through the network URL. Confirm server readiness first. Launcher visibility and installation permissions need verification on physical PS5 hardware. |
| No files or folders appear | Refresh the UI and inspect the readable roots. Browse only mounted paths that really exist, such as `/data` or `/mnt/usb0` when present. Do not bypass permissions with kernel patches. |
| Conversion or extraction is rejected | Check the source type, destination existence, write access, free space, output name, and whether a file or folder with that name already exists. |
| A conversion stopped after a page reload or payload reload | Reloading a payload manager ends the old process, so the original progress bar and in-memory job history disappear. Start the current payload again: it scans `/data/mkpfs-resume`, restores one valid conversion, and shows a new **resuming conversion** job. The UI cannot display work that occurred while the payload was offline. During exFAT creation, recovery continues from the last whole-file checkpoint after validating the unchanged source tree. After the exFAT snapshot is durable, PFSC/PFS work resumes from the private stage without rescanning the source. If recovery reports an invalid checkpoint or source change, start a new conversion from the original source. Temporary files created by older payload versions predate this journal and are not resumable. |
| A URL download fails | Use a final direct URL without sign-in or redirects, check PS5 network access and free space, then submit a new job. Do not disable HTTPS certificate checks to work around a TLS error. |
| Cancellation does not finish immediately | Cancellation is cooperative. Wait for the current read or work unit to finish and for the job to reach a terminal state. |
| A `.ffpfsc` file is not usable | Start with a small source folder, confirm the job completed, check the output size, and test it with the expected MkPFS workflow. The upstream verifier is available for host-side validation. |

## Features

- Port **6777**, kept in sync with the startup notification and managed Home Screen launcher.
- PS5 web file browser with explicit source and destination selection.
- Bounded-memory, deterministic folder-to-`.ffpfsc` conversion.
- RAR/7z extraction with password input, supported multipart archives, staging, progress, and cooperative cancellation.
- Direct HTTP/HTTPS URL downloads with background jobs, bounded streaming, cleanup, and overwrite protection.
- Job status, history, progress, speed, ETA, errors, and cancellation.
- Path validation, traversal rejection, symlink safeguards, no-follow cleanup, collision checks, and same-directory atomic finalization.
- English and Chinese UI strings with D-pad-friendly focus navigation.

## Credits and thanks

I could not have built this project without the work of the open-source projects below. Thank you to their maintainers and contributors.

| Project | How it helps this project |
| --- | --- |
| [PSBrew/MkPFS][1] | Provides the reference implementation and verifier used for `.ffpfsc` compatibility. |
| [owendswang/ps5-web-file-manager][2] | Provides the PS5 runtime architecture, file manager, root discovery, launcher flow, and file-management API foundation. |
| [bizkut/unrar-ps5][3] | Provides the native RAR/7z extraction engine and the basis for archive integration. |
| [7-Zip][4] | Provides the decoder included with the vendored 7z extraction sources. |
| [PS5 Payload SDK][5] | Provides PS5 ELF build tooling and platform API integration. |
| [GNU libmicrohttpd][6] | Provides the embedded HTTP server. |

The required licenses, notices, and attribution are retained in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## References

[1]: https://github.com/PSBrew/MkPFS "PSBrew MkPFS"
[2]: https://github.com/owendswang/ps5-web-file-manager "owendswang PS5 Web File Manager"
[3]: https://github.com/bizkut/unrar-ps5 "bizkut unrar-ps5"
[4]: https://www.7-zip.org/ "7-Zip"
[5]: https://github.com/ps5-payload-dev/sdk "PS5 Payload SDK"
[6]: https://www.gnu.org/software/libmicrohttpd/ "GNU libmicrohttpd"
[7]: https://github.com/rynmrde/mkpfs-ps5/releases/latest "RAR to FFPFSC PS5 Payload releases"
