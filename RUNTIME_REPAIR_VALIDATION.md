# PS5 Runtime Repair and Validation Report

**Author:** Manus AI  
**Repository baseline:** `7d1bb7874c486484bcbcb302f6ee515f18e9d005` (`v0.3.8`)  
**Reference compared:** `owendswang/ps5-web-file-manager` main branch, commit `ad7d75474f2bb3d8fda2ae7acd7d1c86e5acba98` [1]

## Conclusion

The runtime comparison identified two direct causes of the reported launch failure sequence. First, the payload defaulted to **port 6777**, while the proven reference defaults to **8888** and advances to the next port only when a bind fails because the address is already in use. Second, the payload ran launcher installation synchronously inside the HTTP ready callback before issuing the notification. A slow or failing application-install call could therefore delay the startup notification and leave a listener that had already been bound appearing unreachable.

The repair changes only runtime startup, HTTP listener resilience, launcher timing and metadata, detached filesystem-task ownership, documentation, and focused regression coverage. It does **not** change MkPFS conversion, RAR/7z extraction, URL downloading, archive formats, or filesystem operation semantics. The rebuilt ELF is statically and structurally validated, but it has **not been run on PS5 hardware**. This report therefore makes no claim that the notification, launcher installation, or network behavior has been verified on a physical console.

## Focused Line-by-Line Runtime Comparison

| Component | Proven file-manager behavior | Baseline payload behavior | Result and disposition |
|---|---|---|---|
| Startup and requested port | `src/main.c` defaults to `8888`, probes availability from that port upward, and retries if needed. | `src/main.c` defaulted to `6777`; it retried only the configured port after failure. | **Changed.** The payload now defaults to `8888`, retains `WFM_PORT` as an optional starting override, and advances only after `bind()` reports `EADDRINUSE`. |
| HTTP readiness and notification order | The reference creates the listener and MHD daemon before notifying. | The baseline invoked `app_install_if_needed(port)` before `notify_user()` from the ready callback. | **Changed.** The listener now reaches the ready callback, sends the notification, and then starts launcher installation on a detached worker. |
| Payload lifetime after transient socket failures | The reference keeps a manual accept loop around MHD. | The baseline kept that design but terminated the listener for every `accept()` failure other than `EINTR`. | **Changed.** `EAGAIN`, `ECONNABORTED`, descriptor exhaustion, and bounded transient network errors now keep existing MHD connections and filesystem workers alive. Fatal errors still exit the listener and trigger the established retry loop. |
| HTTP server and MHD ownership | The reference binds an `INADDR_ANY` TCP socket, configures MHD with `MHD_USE_NO_LISTEN_SOCKET`, and passes accepted peers into MHD. | The baseline uses the same tested architecture, including `SO_REUSEADDR`, `SO_RCVBUF`, `SO_SNDBUF`, and `TCP_NODELAY`. | **Retained and hardened.** The architecture was already aligned. Per-connection memory was reduced from 8 MiB to 512 KiB, connection limits increased from 8/4 to 32/16 total/per-IP, and the timeout increased from 30 to 120 seconds to reduce ordinary browser-session pressure. |
| Socket reachability from a phone | The reference listens on all interfaces and its frontend uses origin-relative paths. | The baseline also bound `INADDR_ANY`, but startup could be held inside launcher installation and the documented port was 6777. | **Changed only where divergent.** The listener remains on all interfaces. The notification is now sent after MHD startup without waiting for launcher work, and the default URL is `http://PS5-IP:8888/`. |
| Notification implementation | The reference writes the formatted message into `sceKernelSendNotificationRequest` data. | The baseline uses the same request layout and now logs a negative API result. | **Retained.** No incompatible notification structure was found. Only its position in startup was corrected. |
| Launcher asset registration | The reference registers the embedded icon with the HTTP asset table. | The baseline registered the icon only during launcher installation. | **Changed.** `app_register_assets()` executes before MHD can accept a browser or launcher request. Registration is idempotent and the installer reuses it. |
| Launcher installation and metadata | The reference creates `/user/app/<title>/sce_sys`, writes `param.json` and `icon0.png`, then invokes the AppInstUtil installation path. | The baseline uses the same AppInstUtil resolution path, but did the work synchronously in startup. Its generated metadata followed the old port. | **Changed.** The same dynamic installation path is retained, but executes after readiness on a detached worker. The generated `deeplinkUri` receives the actual bound port. Atomic icon replacement is used alongside the existing atomic owned-`param.json` replacement. |
| Static launcher metadata and deeplink | The reference asset metadata contains `http://127.0.0.1:8888/`. | `assets/param.json` contained `http://127.0.0.1:6777/`. | **Changed.** The static metadata now points to `127.0.0.1:8888`; runtime-generated metadata uses `http://127.0.0.1:<bound-port>/` when a fallback port is required. |
| Filesystem initialization and roots | The reference starts the file API without a special initialization stage and routes `/api/list` to `api_list`. | The baseline additionally recovers interrupted conversions, canonicalizes absolute paths, and discovers mounted `/mnt` children. | **Retained.** These additions are conversion and mount-discovery features, not a conflicting initialization mechanism. The repaired host smoke test performs a real authenticated `/api/list` request and verifies `readme.txt` in the returned listing. |
| Frontend/API connection | The reference frontend calls relative paths such as `fetch('/api/list')`, allowing the active browser origin and port to control routing. | The baseline likewise uses relative frontend URLs. Its host-only token handling is disabled for PS5 API requests by `__SCE__`. | **Retained.** No frontend hard-coded port or URL defect was found. This is necessary for the launcher and a phone browser to reach the same currently bound server. |
| Detached filesystem task lifetime | The reference uses detached task threads. | The baseline could remove a terminal task from the shared list during the short interval before its detached worker returned. | **Changed.** `worker_active` prevents cleanup until the owning detached worker exits, avoiding a possible use-after-free during task completion. |

> **Runtime sequence after repair:** payload entry starts → interrupted conversion recovery runs → listener binds `8888` or the next free port → MHD starts → notification reports the bound port → launcher registration proceeds independently → browser and launcher both use the same origin and port → `/api/list` serves the filesystem.

## Implementation Scope

The following source and support files changed. The change list is deliberately limited to the runtime boundary and its regression coverage.

| File | Purpose of change |
|---|---|
| `src/main.c` | Default port, `EADDRINUSE` fallback, notification ordering, and detached launcher worker. |
| `src/websrv.c` | Bounded listener resource settings and recoverable `accept()` handling. |
| `src/app_installer.c`, `src/app_installer.h` | Early idempotent icon registration and atomic launcher icon writing. |
| `src/filemgr.c`, `src/filemgr_internal.h`, `src/task.c` | Detached worker ownership guard during terminal task cleanup. |
| `assets/param.json` | Static launcher deeplink default of `127.0.0.1:8888`. |
| `tools/test_default_port.sh` | Default-port and occupied-port fallback regression test. |
| `tools/smoke_http.sh` | HTTP frontend, filesystem-listing, and conversion API smoke coverage. |
| `README.md` | Runtime URL and launcher behavior documentation. |
| `rar-to-ffpfsc-ps5-payload.elf.sha256` | Checksum for the fresh ELF described below. |

## Regression and Build Results

The host build and regression commands completed against the repaired source. The first archive-test attempt exposed missing local host utilities (`g++`, `rar`, and `7z`), not a source failure. Those dependencies were installed, and the affected test set was rerun successfully.

| Validation | Result | Evidence |
|---|---|---|
| Working-tree hygiene | Passed | `git diff --check` completed without whitespace errors. |
| Host Linux build | Passed | `make linux` completed with `-Wall -Werror`. |
| Native MkPFS conversion | Passed | `make test-native`. |
| Native MkPFS resume | Passed | `make test-resume`. |
| RAR/7z, encrypted, and multipart extraction | Passed | `make test-archive`. |
| Conversion restart recovery | Passed | `make test-conversion-recovery`. |
| exFAT restart recovery | Passed | `make test-exfat-recovery`. |
| Changed-source recovery rejection | Passed | `make test-exfat-source-change`. |
| URL downloader | Passed | `make test-url-download`. |
| Default port and occupied-port fallback | Passed | `make test-default-port` printed `DEFAULT_PORT_8888_AND_FALLBACK_PASS`. |
| Upstream folder compatibility | Passed | `MKPFS_UPSTREAM_ROOT=/home/ubuntu/MkPFS make compat-upstream` verified the native output with PSBrew MkPFS and reported zero warnings and zero errors. |
| HTTP frontend, filesystem listing, and conversion smoke | Passed | `tools/smoke_http.sh` loaded `/`, verified `/api/list` returned `readme.txt`, completed a conversion task, and rejected a traversal-form source. |
| PS5 target build | Passed | `make -j2` with `PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk`. |

## Exact ELF Validation

The fresh output is `rar-to-ffpfsc-ps5-payload.elf`. It was built after the repaired source had passed host regressions. Structural inspection confirms a 64-bit little-endian x86-64 position-independent ELF. Its dynamic dependencies include `libSceNet.sprx`, `libSceAppInstUtil.sprx`, and `libSceHttp.sprx`. The ELF contains the notification title, `Port: %u`, `listening on port %u`, and the dynamic launcher deeplink template `http://127.0.0.1:%u/`.

| Property | Value |
|---|---|
| SHA-256 | `945d633e9f6dc4df42d0354c17b72a909a8173fb49b18a63fb4098e7d58bd495` |
| Size | 2,246,192 bytes (approximately 2.2 MiB) |
| ELF type | `DYN` position-independent executable |
| Architecture | `Advanced Micro Devices X86-64` |
| Exact validation result | `PS5_ELF_VALIDATION_PASS` |

## Hardware Verification Boundary

The following claims remain unverified because no PS5 console is available in this environment: successful delivery of the actual system notification, Home Screen tile appearance, AppInstUtil refresh behavior on the target firmware, phone reachability across the console network, and sustained filesystem access on mounted PS5 storage. The repaired runtime is designed so that these hardware checks can be performed independently: the notification reports the exact listener port, the phone URL uses that port, and launcher work cannot block the listener.

A physical-console test should load **only the attached ELF**, wait for the startup notification, browse to `http://PS5-IP:8888/` unless the notification reports a fallback, call up a filesystem listing, and then open the launcher. This is a recommended validation procedure, not a report of completed hardware testing.

## References

[1]: https://github.com/owendswang/ps5-web-file-manager "PS5 Web File Manager"
[2]: https://github.com/ps5-payload-dev/sdk "PS5 Payload SDK"
