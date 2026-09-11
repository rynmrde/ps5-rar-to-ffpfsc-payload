# MkPFS-PS5 v0.4.6 Audit Report

## Scope and branch

This audit was performed from the `v0.4.6` `main` commit `158a57ea39be12df3733414547536b4984674113` on the required branch `audit/launcher-and-critical-fixes`. The audit covers the download state machine, HTTP API method policy, task retention, embedded launcher assets, folder conversion, archive extraction, package installation, and the web server request path.

## Phase 1: Applied fixes

### Download resume corruption

`src/url_download.c` now accepts an HTTP `200 OK` response when a resumed request receives a server that ignores `Range`. In both the PS5 and host HTTP paths it truncates the temporary file with `ftruncate(fd, 0)`, seeks back to offset zero, resets the task progress and checkpoint counters, rewrites the journal, and proceeds as a fresh download. HTTP `206 Partial Content` remains the normal resumed-transfer response; other incompatible statuses are rejected.

### HTTP method enforcement

`src/filemgr.c` now includes `/api/download/prepare` in the primary POST-only method gate. A GET request to that state-changing endpoint therefore returns `405 Method Not Allowed` before the handler is called.

### Task retention and concurrent polling

The destructive cleanup call was removed from the end of `api_tasks()`. Cleanup now occurs before serialization and uses a 60-second TTL for `TASK_DONE` and `TASK_FAILED` tasks. Active, paused, worker-owned, streamed, and unreported package-install tasks remain protected. This allows concurrent polling clients to observe terminal state without the first poller deleting it immediately.

## Phase 2: Launcher deployment

`assets/index.html` was replaced with the specified Glassmorphism PS5-style launcher. The new asset includes the requested CSS variables, glass cards, task polling through `/api/tasks`, progress rendering, and D-pad arrow navigation. `gen-asset-module.py` was executed for the asset set; the generated `gen/index.html.c` registers `/index.html` with gzip encoding, and the final Linux/PS5 build consumed that generated module.

The launcher UI's URL-download and archive-extraction buttons remain the exact supplied placeholders (`alert(...)`), while the file-manager button navigates to `/fs` as specified. These are presentation-level limitations of the supplied HTML rather than changes to the C API.

## Phase 3: Verification

| Check | Result |
|---|---|
| PS5 payload build with `PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk` | PASS |
| Linux host build with `-Wall -Werror` | PASS |
| Native MkPFS conversion tests | PASS: `mkpfs-native tests passed` |
| Resumable conversion tests | PASS: `mkpfs resumable conversion tests passed` |
| URL HTTP integration regression | PASS |
| HTTP 200 ignored-Range resume regression | PASS: added to `tools/test_url_download_http.sh` and fixture server |
| URL restart/recovery regression | PASS |
| Default-port/fallback regression | PASS |
| JavaScript syntax check | PASS |
| Generated launcher asset registration | PASS |
| Targeted cppcheck for `url_download.c`, `filemgr.c`, and `task.c` | PASS: no diagnostics |
| ELF format | PASS: x86-64 Linux host artifact from the host build; PS5 cross-build also completed earlier in the release sequence |

The clean rebuild produced host ELF SHA-256 `1f6e13be8967b496a52472610160d2ad8bb54a7d3e919c3027295de7154d20d3`. The release artifact already published for v0.4.6 remains the PS5 ELF associated with tag `v0.4.6`; the host rebuild is a verification artifact and is not presented as a PS5 payload.

## Phase 4: Deep audit findings

### Folder conversion (`src/mkpfs_native.c`)

The conversion and resumable-publish paths consistently close `FILE *`/file descriptors on normal and failure paths. Temporary output is removed on failure, allocated inode/root buffers are freed on both success and failure, and cancellation is checked during the conversion/publish stages. No new descriptor or heap leak was identified in the reviewed exFAT creation paths. Native and resumable conversion tests passed after the review.

### Archive extraction and path traversal

The file-manager extracts into a task-specific staging directory under the destination parent and removes the staging tree when extraction fails or is canceled. `relative_path_safe()` rejects absolute paths, empty components, `.`/`..` components, and backslashes; `path_join()` and `path_join_relative()` enforce that predicate. The reviewed file-manager destination construction therefore has a containment check before publication. A remaining audit limitation is that the embedded third-party RAR/7z writer internals were not rewritten: malicious archive-entry handling should continue to be covered by dedicated traversal fixtures on real PS5 and host builds.

### Package installer

The installer serializes package operations under `installer_lock`, and the task worker owns task completion reporting. The significant residual risk is in launcher refresh: `app_install_if_needed()` writes and syncs the replacement metadata, then calls `sceAppInstUtilAppUnInstall()` before `install_app()`. If the subsequent install or Home Screen registration fails, the existing registered launcher may already have been removed. This is a recoverability/design finding, not silently marked fixed, because the correct firmware-specific replacement/registration transaction is not available in the host environment.

### Web server

`websrv_on_request()` accepts only GET, POST, and HEAD, allocates a bounded per-connection context, enforces authorization for API requests on host builds, and routes body data through configured limits. The daemon uses bounded connection/memory limits and handles recoverable accept errors without tearing down active work. No header-injection or obvious request-buffer overflow was found in the reviewed path. Continued fuzzing of malformed headers and URLs on a PS5-compatible runtime is recommended.

## Remaining findings and boundaries

The audit found one material operational risk: launcher refresh can leave the title unregistered after uninstall succeeds but install/refresh fails. It is documented above and should be addressed with a firmware-verified transactional strategy before treating launcher refresh as failure-atomic. Physical PS5 runtime behavior, AppInstUtil registration state, and Home Screen refresh cannot be simulated by the Linux host binary; they require a real PS5 with the corresponding payload loader and firmware.

## Commits on this audit branch

1. `d3d638f` — `fix: harden download resume and task lifecycle handling`
2. `da5b588` — `feat: deploy Glassmorphism PS5 launcher UI`

The branch is intentionally separate from `main`; no release tag was moved by this audit branch.
