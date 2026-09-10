# MkPFS-PS5 v0.4.4

## Verified fixes

This release is based on the independently audited `reliability-v0.4.3` branch at commit `f1b42cf16fa9e971f34d55c2c7142dffd8519d50`, not on stale `main`.

The directory-list API no longer rejects read-only listings merely because another task is active. Path validation, containment checks, and filesystem error handling remain unchanged.

Failed URL-download tasks remain available for history and retry, but no longer count against the bounded URL-download queue limit. Active and paused URL-download tasks continue to consume queue slots, and the existing worker and retry state machine is preserved.

Launcher publication is idempotent for healthy owned assets. Exact matching `param.json` and `icon0.png` files are left in place without uninstall/reinstall or repeated refresh work. Missing, corrupted, or port-mismatched owned assets are still atomically replaced before the payload title is refreshed. The actual selected listener port remains the single value passed to readiness notification and launcher metadata.

The MkPFS/FFPFSC conversion format and bounded-memory conversion pipeline were not modified.

## Verification

Host-side native conversion, archive, downloader, recovery, HTTP, filesystem, sanitizer, Valgrind, static-analysis, and configured upstream MkPFS compatibility checks passed. The resume test was corrected so resource-closing and conversion/checkpoint calls are evaluated outside assertions; `cppcheck` now completes with no findings under the configured warning, performance, and portability checks.

A real PS5 ELF could not be built in this environment because `PS5_PAYLOAD_SDK` and the Prospero toolchain are unavailable. Consequently, no ELF checksum or PS5 release asset is claimed by this source-audit result.

No physical PS5 hardware was available. Home Screen registration, deeplink launch, notification delivery, LAN/mobile access on console hardware, controller usability, and multi-hour physical payload uptime therefore remain hardware validation items rather than claimed results.
