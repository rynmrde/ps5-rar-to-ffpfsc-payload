# MkPFS-PS5 v0.4.3 Release Audit

## Scope and root cause

The audit began from the published v0.4.2 ELF (`b7df3f1604e62ba150ebb1c09bf18a0becd1f95d9e69ac4507d0015429b4c1f6`) and its exact v0.4.2 source tag. The target was audited directly, including its listener loop, task lifecycle, conversion checkpoints, downloader recovery, archive paths, frontend polling, installer, and PS5-only imports.

The newly corrected source defect was in the Home Screen refresh sequence. `app_install_if_needed()` unregistered the payload's existing `FMGR88888` title *before* creating and persisting replacement metadata and icon files. Any storage, path, or I/O failure after that call left a running server but removed the launcher. This is a concrete failure path for a missing launcher, not a host-only theory.

## Fix

The installer now writes the owned launcher metadata and icon atomically, fsyncs the containing directory after every new or replaced file, and only then unregisters the payload's own title immediately before re-registration. It never removes unrelated titles. This preserves the existing dynamically resolved `sceAppInstUtilAppInstallTitleDir` flow and `AppInstallAll` fallback while ensuring AppInstUtil sees a durable replacement directory.

The audit also confirmed the existing v0.4.2 listener behavior is already correct: default port 8888, incremental occupied-port fallback, readiness notification/launcher metadata use of the actual bound port, bounded MHD connections, retryable accept errors, and rejected `MHD_add_connection` peers do not terminate the daemon. Conversion and downloader startup recovery calls remain present in `main.c`.

## Verification performed

| Area | Result |
|---|---|
| PS5 source syntax with PS5 SDK, libmicrohttpd, and zlib | Passed |
| Clean final PS5 ELF build | Passed |
| ELF architecture/type/import validation | Passed: x86-64 PIE/DYN; Sce AppInstUtil and Sce HTTP imports present |
| Final ELF runtime markers | Passed: v0.4.3, `WFM_PORT`, MHD startup, launcher uninstall, notification symbols |
| Native PFSC and resume tests | Passed |
| RAR, multipart RAR, 7z, multipart 7z, encrypted archive tests | Passed |
| HTTP/default-port/filesystem-security tests | Passed |
| Downloader queue, pause/resume, cancellation, and restart recovery | Passed |
| Conversion, PFSC-stage restart, exFAT-stage restart, and changed-source rejection | Passed |
| Frontend JavaScript syntax | Passed |
| Static analysis | Completed; no correctness blocker in the audited changed launcher path |

## Final artifact

`rar-to-ffpfsc-ps5-payload.elf` is a new v0.4.3 artifact, 2,442,800 bytes.

```text
c9d202cef5467513eb114e52879f9e6a832c50271cc6c0ed9a13b3dc10703865  rar-to-ffpfsc-ps5-payload.elf
```

## Physical PS5 verification still required

No physical PS5 is attached to this environment. The final ELF was built and inspected through the PS5 SDK, but the following must be exercised on an actual jailbroken PS5 before claiming hardware behavior: `sceAppInstUtil` refresh on the target firmware, Home Screen rendering and deeplink launch, notification delivery, LAN reachability from a phone, controller interaction in the PS5 browser, and multi-hour conversion uptime.
