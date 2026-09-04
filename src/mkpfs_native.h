#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mkpfs_scan_result {
  uint64_t total_bytes;
  uint64_t file_count;
  uint64_t directory_count;
} mkpfs_scan_result_t;

typedef struct mkpfs_native_options {
  uint32_t block_size;
  uint32_t inode_bits;
  uint32_t compression_level;
  uint32_t workers;
  uint64_t minimum_compressible_size;
  int target_ps5;
  int compression;
  int verify;
  int verify_structure;
} mkpfs_native_options_t;

/* Normalizes an absolute PS5 path and rejects traversal or malformed input. */
int mkpfs_normalize_path(const char *input, char *output, size_t output_size);

/* Bounded-memory recursive scan used by the web UI and conversion preflight. */
int mkpfs_scan_folder(const char *root, mkpfs_scan_result_t *result);

/*
 * Deliberately returns ENOTSUP until the complete MkPFS on-disk writer is
 * ported. It must never create a file that merely has a .ffpfsc suffix.
 */
int mkpfs_convert_folder(const char *source, const char *destination,
                         const char *output_name,
                         const mkpfs_native_options_t *options,
                         volatile int *cancel_requested);

#ifdef __cplusplus
}
#endif
