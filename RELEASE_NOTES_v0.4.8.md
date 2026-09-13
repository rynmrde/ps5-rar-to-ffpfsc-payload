# MkPFS-PS5 v0.4.8 - UI Completion & Critical Fixes

## Highlights

- **Feature:** Completed the Glassmorphism launcher wiring for File Manager, URL Downloader, Archive Extractor, and FFPFSC Converter actions, including their browser flows and backend API integration.
- **Feature:** Added controller-friendly modal implementations for browsing sources and destinations, entering download and extraction parameters, selecting conversion options, and displaying task feedback.
- **Feature:** Improved D-pad focus management across cards, dialogs, file rows, browser controls, and task actions so the launcher remains usable from the PS5 browser with a controller.
- **Safety:** Made launcher refresh transactional: the payload removes and re-registers only its own launcher entry, verifies the newly generated metadata, and preserves rollback safety when publication or registration fails.
- **Hardening:** Strengthened download transfer handling, extraction containment, task-list synchronization, and error-path resource ownership.

## Fixes

- **Launcher wiring:** Connected the quick-action cards and modal controls to the file-manager, URL-download, extraction, and folder-conversion APIs. Added consistent loading, error, cancellation, and success states so incomplete UI paths no longer leave users without feedback.
- **Modal implementations:** Completed source and destination browsing, URL validation, output-name handling, archive-password input, worker selection, conversion-profile controls, and task-result presentation. Dynamic values continue to be rendered safely through `textContent`; static template literals are the only use of `innerHTML`.
- **D-pad focus updates:** Added predictable focus movement, focus restoration after modal close, dialog trapping, and controller activation behavior for the launcher, file browser, and task list.
- **Transactional launcher refresh:** Launcher metadata publication now uses owned temporary data and rollback-safe replacement. A failed refresh does not discard the previously working launcher state, and registration targets the payload’s own title ID.
- **Download file descriptors:** Audited download setup, response, cancellation, and failure paths so descriptors are closed exactly once, including early HTTP and conflict failures.
- **Download header parser:** Hardened response-header parsing for case variations, whitespace, malformed values, duplicate fields, and bounded header data rather than trusting unchecked input.
- **Content-Length validation:** Added validation for missing, malformed, negative, overflowing, and inconsistent `Content-Length` values before allocating or committing transfer state. Resume handling distinguishes valid `206 Partial Content` responses from a server that incorrectly returns `200 OK`.
- **Task-list thread safety:** Protected task snapshots and terminal-task cleanup with the task mutex, retained terminal results for the configured observation window, and avoided formatting live task memory after releasing ownership.
- **Extraction containment:** Added final `lstat`-based staging-tree verification so symlinks, directory symlinks, FIFOs, hardlinks, sockets, devices, and other unsafe entries cannot be published. Archive nesting is capped at 128 levels and fails closed with `ELOOP` before recursive stack exhaustion.
- **Error-path ownership:** Corrected busy-conflict cleanup and extraction/conversion failure cleanup so transferred source arrays and strings are released exactly once without double-free or leak behavior.

## Features

- Responsive Glassmorphism launcher for phone, desktop, and PS5 browser use.
- File Manager, URL Downloader, Archive Extractor, and FFPFSC Converter quick actions.
- Real-time task polling with queued, running, completed, failed, and canceled states.
- Controller-oriented D-pad navigation, Cross/Enter activation, Backspace/Alt+Up parent navigation, and refresh shortcuts.
- Atomic extraction publication and streamed, bounded folder conversion with cancellation and recovery support.
- Dynamic mount discovery under `/mnt` while retaining absolute-path and symlink safety boundaries.

## Verification boundary

The Linux build and host regression suite validate portable conversion, downloader, recovery, archive, API, and task behavior. The PS5 ELF requires the PlayStation 5 payload SDK and platform libraries. Physical AppInstUtil registration, Home Screen refresh, notification delivery, firmware-specific mount visibility, and long-running runtime stability require testing on a jailbroken PS5 with the target firmware.
