# MkPFS-PS5 v0.4.0

## Reliability release

This release restores the PS5 Web File Manager’s established **port 8888** default and sequential port fallback. A port is announced only after the listener and HTTP daemon have both initialized. The launcher registration path is moved behind server readiness, uses the actual selected port, updates only this payload’s owned title files atomically, and uses the **MkPFS-PS5** identity and original launcher icon.

The HTTP service now has bounded per-connection memory and socket buffers, practical connection limits, a longer request timeout for console-browser transfers, and recovery from transient accept errors and temporary descriptor exhaustion without tearing down existing work.

The folder-conversion request no longer performs its source scan and storage calculation in the HTTP request thread. It queues the task first; all heavy validation remains before converter writes. Existing output names remain protected. Existing conversion and exFAT recovery journals remain unchanged and were regression-tested.

RAR, multipart RAR, 7z, multipart 7z, password-protected RAR, and password-protected 7z continue to use the embedded upstream extraction adapter. Password-related failures now receive a dedicated task error classification.

URL downloads now use a bounded two-worker queue with a maximum of eight retained URL jobs. Downloads can pause, resume, retry after a transient failure, and recover after payload restart. A same-directory private part file is paired with an atomic journal. Recovery truncates only an unjournaled trailing suffix and makes a validated HTTP Range request; partial final filenames are never published. Destination collisions, redirects, credentials, cookies, custom headers, and TLS-verification bypasses remain rejected or unsupported by design.

## Verification performed

The release was built as a Linux integration server and as a fresh PS5 ELF against the official PS5 Payload SDK v0.43. Host validation passed: native MkPFS conversion and resume tests; conversion and exFAT restart recovery; changed-source recovery rejection; authenticated filesystem API safety; HTTP robustness; default-port and occupied-port fallback; direct HTTP URL download, cancellation, pause/resume, and crash/restart range recovery; and single-volume, multipart, and password-protected RAR/7z extraction.

Physical execution on a jailbroken PS5 was not available in this environment. The ELF has been cross-built and linked against target SDK imports, but console notifications, launcher visibility, mount behavior, SceHttp TLS behavior, and prolonged hardware stability must be verified on the target firmware before being represented as hardware-tested.
