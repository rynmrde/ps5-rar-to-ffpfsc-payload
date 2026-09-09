# MkPFS-PS5 v0.4.1

## Reliability and lifecycle correction

This corrective release keeps the HTTP service alive across recoverable PS5 `accept()` failures. The listener loop now follows the proven NanoDNS daemon-loop behavior: transient socket errors are logged and retried without stopping libmicrohttpd, active requests, or background conversion/download work. Explicit stop requests still terminate the service normally.

Completed detached task workers now retain ownership of their task record until the worker has returned. This closes a race in which `/api/tasks` cleanup could free a terminal task while its worker was still executing its final instructions, causing long-running instability and possible payload termination.

The v0.4.0 port, launcher, notification, extraction, conversion, `.ffpfsc`, restart-recovery, downloader, and filesystem-safety behavior is otherwise unchanged.

## Verification

Host verification covered native conversion and resume, conversion and exFAT restart recovery, changed-source rejection, RAR/7z including multipart and password-protected archives, HTTP robustness, filesystem API safety, port fallback, URL download pause/retry/cancel and restart recovery, repeated idle/API requests, Clang syntax analysis, ASan/UBSan, and Valgrind. A fresh PS5 ELF was cross-built with the official PS5 Payload SDK and its SHA-256 is recorded with the release artifact.

Physical execution on jailbroken PS5 hardware was not available in this environment. Firmware-specific notification rendering, Home Screen visibility, mount behavior, SceHttp behavior, and prolonged hardware stability remain hardware-only validation items.
