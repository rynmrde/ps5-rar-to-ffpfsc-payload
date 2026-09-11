# MkPFS-PS5 v0.4.7 - Critical Fixes & Next-Gen UI

## Highlights

- **Fix:** Resolved URL download resume corruption when servers return HTTP 200 instead of HTTP 206 for a resumed request.
- **Fix:** Fixed the `/api/tasks` cleanup race with a 60-second terminal-task TTL.
- **Fix:** Enforced strict POST methods for state-changing APIs, including download preparation.
- **Feature:** Deployed the Glassmorphism Web Launcher with D-Pad support and real-time task polling.
- **Feature:** Added the **FFPFSC Converter** quick action for converting extracted folders into optimized `.ffpfsc` images through the file-manager conversion flow.
- **Audit:** Passed the host regression suite, asset regeneration checks, and targeted memory/descriptor and static-analysis review.

## Verification boundary

The v0.4.7 ELF is built from the integrated `main` branch with the PS5 payload SDK. Linux host tests validate the portable conversion, downloader, recovery, and API behavior. Physical PS5 AppInstUtil registration, Home Screen refresh, and firmware-specific runtime behavior require a real PS5 and are not simulated by the host build.
