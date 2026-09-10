# Runtime Repair Validation — v0.3.10

**Scope.** This release corrects the PS5 runtime path from payload load through browser access, filesystem listing, and Home Screen launcher registration. It is an implementation repair, not an audit-only result. The current source was compared component-by-component with the proven [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) implementation at upstream commit `ad7d754`; MkPFS output was verified with [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) commit `78eda0a`.

## Root causes and corrected behavior

| PS5 symptom | Source-level root cause | Corrective implementation |
| --- | --- | --- |
| Notification appears but the browser is unreachable, especially after reloads | The payload used non-upstream port `6777`, had no sequential bind fallback, and did not replace an existing same-named payload process. A stale payload could retain the expected listener while a fresh payload either retried the same port or reported a misleading state. | `main.c` now restores the upstream process-replacement model and uses port **8888** by default. It attempts `8889`, `8890`, and subsequent ports only after `bind(2)` returns `EADDRINUSE`. The post-bind callback receives the actual listener port. |
| Phone/PS5 browser requests and directory/API responses are intermittent | The custom daemon limited the whole server to eight connections, four connections per IP, and thirty seconds per connection. These limits are not part of the proven upstream serving model and can reject or time out browser assets and API requests under ordinary PS5 browser use. | `websrv.c` restores the upstream-style thread-per-connection MicroHTTPD configuration, removes the artificial global/per-IP/timeout rejection path, uses `SOMAXCONN`, preserves the direct socket ownership model, and sends CORS plus no-store response headers. |
| Filesystem listing can fail even while the frontend loads | Rejected or timed-out MicroHTTPD connections could remove the list/API request from the same small connection budget as static assets. | The restored daemon model accepts the browser/API workload without those artificial connection caps. The HTTP smoke test now explicitly authenticates and verifies `/api/list` before conversion. Existing upstream-compatible root discovery and `lstat`-based directory enumeration were retained. |
| Home Screen launcher is missing, stale, or opens a blank/outdated page | Launcher work ran synchronously in the ready callback and custom code uninstalled the title before re-registering it. A slow or failed AppInstUtil path could delay the service path, and removal before successful registration could leave no launcher. | The listener is marked ready first; notification is sent for the actual port; launcher installation runs in a detached worker so it cannot block accepted browser connections. Title-directory registration is restored without uninstalling the existing title. Icon registration occurs before daemon startup, and `param.json`/icon updates are atomically written with a runtime deeplink of `http://127.0.0.1:<actual-port>/`. |
| Long-running background work can destabilize the runtime after terminal task pruning | A detached worker could return through an early error path after the task had become terminal, while the cleanup path could prune the task object. | `filemgr.c`, `task.c`, and `filemgr_internal.h` now retain task storage until a detached worker has returned on every exit path. This protects the MkPFS/RAR/downloader runtime integration without changing its behavior. |

## Exact source changes

The release changes `VERSION_TAG` to **v0.3.10**. `main.c` now uses the dedicated, exact 14-character service token `mkpfs-svc-7c91` for PS5 stale-payload detection rather than the 29-character ELF/display name, which cannot fit in the kernel thread-name field. `process_identity.c` validates every variable-size `KERN_PROC_PROC` record before reading its PS5 ABI fields, uses `memcpy` rather than unaligned casts, rejects malformed or unterminated records, and asserts the target SDK `kinfo_proc` offsets, sizes, and types at PS5 build time. Only an exact token match from a complete record is considered for self-excluded stale-instance cleanup. The normal bind-result-driven fallback remains available if cleanup safely declines a malformed process listing.

`list.c` now tracks hard-coded roots actually emitted before discovery and omits only their duplicate `/mnt/*` entries; firmware/device-dependent directory discovery is retained. The filesystem/API audit asserts that `/api/roots` has no duplicate paths. The conversion, RAR/7z extraction, URL downloader, recovery, branding, launcher, notification, port, UI, and security paths are retained unchanged.

Regression coverage was extended so that the host server must prove default `8888`, explicit override, invalid/zero fallback to `8888`, and sequential fallback from an occupied port to the next port. The HTTP smoke test now verifies authenticated directory listing as well as an end-to-end conversion.

## Verification

| Validation | Result |
| --- | --- |
| Host build | A clean `make linux` passed with the new process-record parser linked. |
| Full host runtime suite | Passed: native MkPFS, resumable conversion, process-record parser malformed-buffer cases, RAR/7z extraction (including encrypted and multipart archives), conversion restart recovery, exFAT restart recovery, changed-source rejection, downloader HTTP integration, default-port/fallback regression, and HTTP conversion/listing smoke. |
| Filesystem API security regression | `FILESYSTEM_API_SAFETY_PASS`: authentication, root-alias deletion rejection, method validation, source-descendant alias rejection, collision rejection, free-space guard, conversion, authenticated browse, and unique `/api/roots` output all passed. |
| HTTP robustness under ASan/UBSan | `HTTP_ROBUSTNESS_PASS server_exit=-15 requests=9` passed. |
| Memory safety | ASan/UBSan passed native MkPFS, process-record parser, archive extraction, full server robustness, downloader, conversion recovery, and exFAT recovery. Valgrind reported **0 errors** and **0 leaked bytes** for native MkPFS and the process-record parser. |
| Static checks | `git diff --check`, `clang-18 --analyze`, and `cppcheck --enable=warning,performance,portability --error-exitcode=1` passed for the corrected runtime sources. |
| MkPFS compatibility | `make compat-upstream` passed against PSBrew/MkPFS; the verifier reported **Warnings: 0** and **Errors: 0** for generated `.ffpfsc` output. |
| PS5 cross-build | A fresh ELF built successfully using PS5 Payload SDK and target `libmicrohttpd`/zlib dependencies. Target SDK assertions verified the `kinfo_proc` offsets/types used for `ki_structsize`, `ki_pid`, and `ki_tdname`. ELF validation confirmed ELF64, little-endian x86-64 PIE, stripped release form, and embedded `v0.3.10`, `FMGR88888`, the service token, and notification port format. |

## Release artifact

| Item | Value |
| --- | --- |
| Release | `v0.3.10` |
| ELF | `rar-to-ffpfsc-ps5-payload.elf` |
| SHA-256 | `811a36217ba73981b1dc80bf463ba225db400932781cd2f99ab14bfa3cb521a0` |
| Default access URL | `http://PS5-IP:8888/` |
| Fallback rule | `8889`, `8890`, and subsequent ports only when the immediately prior bind reports `EADDRINUSE`; use the port in the PS5 notification. |

## Physical PS5 verification still required

Host and cross-build validation cannot prove Sony firmware behavior. On a physical PS5, load the published ELF and verify that the notification reports the selected port, a LAN client reaches `http://PS5-IP:8888/` (or the notified fallback), directory listing appears for readable roots, the Home Screen entry appears without deleting unrelated titles, and that entry opens the same UI on the selected loopback port. Also verify a handoff while a prior v0.3.10 payload instance is present, safe fallback while a legacy v0.3.9 listener occupies port 8888, and that an unrelated process with a different or merely similarly prefixed thread name is never signaled. These checks are specifically required because AppInstUtil/Home Screen refresh behavior, target kernel process enumeration, and console network state cannot be emulated by the host test environment.
