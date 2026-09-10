# MkPFS-PS5 v0.4.5

## Critical Fix
Resolved a fatal state-machine trap in the restored v0.4.3 downloader logic. Previously, failed URL downloads were retained for the "Retry" UI but incorrectly counted against the strict 8-task queue limit (`URL_DOWNLOAD_QUEUE_LIMIT`). Furthermore, the `api_cancel` endpoint ignored `TASK_FAILED` states, making it impossible for users to clear failed tasks without restarting the payload. 

In v0.4.5:
- Failed downloads no longer block new downloads from starting.
- Users can now explicitly cancel/clear failed downloads from the UI, which correctly transitions them to `TASK_CANCELED`, triggers `url_download_discard_state()`, and frees the queue.

No changes were made to the `.ffpfsc` format, conversion pipeline, or launcher idempotence logic.
