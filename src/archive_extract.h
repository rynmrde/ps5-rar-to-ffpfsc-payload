#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*archive_extract_progress_cb)(unsigned int percent,
                                            const char *current,
                                            void *opaque);

/*
 * Extract a RAR or 7z archive into an already-created private staging
 * directory.  The upstream unrar-ps5 decoder performs member path checks;
 * the caller owns validating the archive and final destination containment.
 * Returns a RAR_EXIT-compatible code (0 and 1 are non-fatal), or EINVAL for
 * an unsupported extension.  Cancellation is cooperative and returns the
 * upstream user-break code when the decoder reaches a safe callback point.
 */
int archive_extract_run(const char *archive_path, const char *staging_path,
                        const char *password, unsigned int workers,
                        volatile int *cancel_requested,
                        archive_extract_progress_cb progress, void *opaque,
                        char *error, unsigned long error_size);

#ifdef __cplusplus
}
#endif
