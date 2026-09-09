# MkPFS-PS5 v0.4.2

## Audit corrective release

This release contains the independently audited reliability and consistency fixes for the v0.4.1 implementation.

### Fixed issues

- **URL-download worker-slot leak:** an early cancellation after a URL worker acquired a concurrency slot could leave the global slot count permanently occupied. Every task-worker exit now releases an acquired URL slot before relinquishing task ownership.
- **Version drift:** the browser UI and downloader user-agent now use the v0.4.2 release identity instead of stale v0.3.7/0.4 labels.
- **Non-JSON API failures:** the browser now reports useful HTTP/status errors when an API failure response is not JSON, rather than surfacing a JSON parser exception.

The existing conversion, parallel compression, restart recovery, RAR/7z extraction, downloader, file-manager, launcher, notification, and PS5 UI functionality is preserved.

## Verification

The release was validated with clean host and PS5-target builds, native and integration regression tests, archive and downloader tests, HTTP and filesystem audits, sanitizer/Valgrind checks, stress/resource monitoring, frontend syntax and generated-asset checks, PFSC byte-identity checks against the prior implementation, and performance benchmarks. Host-only results do not replace physical PS5 verification; final launcher installation, ShellUI navigation, SCE HTTP behavior, and long-uptime behavior still require a jailbroken PS5 test unit.
