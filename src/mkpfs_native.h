#pragma once

#include <stddef.h>
#include <stdatomic.h>
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

typedef int (*mkpfs_progress_callback)(uint64_t done, uint64_t total,
                                        const char *phase,
                                        const char *current, void *opaque);

int mkpfs_normalize_path(const char *input, char *output, size_t output_size);
int mkpfs_scan_folder(const char *root, mkpfs_scan_result_t *result);

/* Pack an already-built logical PFS image into the upstream PFSC container. */
int mkpfs_pack_pfsc_file_ex(const char *input_path, const char *output_path,
                            int compression_level, unsigned int workers,
                            const atomic_int *cancel_requested,
                            mkpfs_progress_callback progress, void *opaque);
int mkpfs_pack_pfsc_file(const char *input_path, const char *output_path,
                         int compression_level, const atomic_int *cancel_requested,
                         mkpfs_progress_callback progress, void *opaque);

/* Verify PFSC header, offset table, zlib blocks, and logical size. */
int mkpfs_verify_pfsc_file(const char *path, uint64_t *logical_size,
                           uint64_t *block_count);

/* Build the upstream-compatible four-inode PFS wrapper around a raw exFAT file. */
int mkpfs_wrap_exfat_file_ex(const char *exfat_path, const char *output_path,
                             const char *inner_name, int compression_level,
                             unsigned int workers,
                             const atomic_int *cancel_requested,
                             mkpfs_progress_callback progress, void *opaque);
int mkpfs_wrap_exfat_file(const char *exfat_path, const char *output_path,
                          const char *inner_name, int compression_level,
                          const atomic_int *cancel_requested,
                          mkpfs_progress_callback progress, void *opaque);

int mkpfs_build_exfat_folder(const char *source, const char *output_path,
                              const atomic_int *cancel_requested,
                              mkpfs_progress_callback progress,
                              void *opaque);

int mkpfs_convert_folder_progress(const char *source, const char *destination,
                                    const char *output_name,
                                    const mkpfs_native_options_t *options,
                                    const atomic_int *cancel_requested,
                                    mkpfs_progress_callback progress,
                                    void *opaque);

int mkpfs_convert_folder(const char *source, const char *destination,
                         const char *output_name,
                         const mkpfs_native_options_t *options,
                         const atomic_int *cancel_requested);

#ifdef __cplusplus
}
#endif
