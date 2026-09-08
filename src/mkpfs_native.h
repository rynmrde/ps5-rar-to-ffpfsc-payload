#pragma once

#include <limits.h>
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

/* A resumable conversion exposes progress only after bytes have been flushed
 * and synchronized to its private staging files.  Callers persist this small
 * state atomically and can safely restart from its stated phase after the
 * payload process is lost. */
typedef enum mkpfs_resume_phase {
  MKPFS_RESUME_EXFAT = 1,
  MKPFS_RESUME_PACK = 2,
  MKPFS_RESUME_VERIFY = 3,
  MKPFS_RESUME_PUBLISH = 4,
  MKPFS_RESUME_DONE = 5,
} mkpfs_resume_phase_t;

typedef struct mkpfs_resume_state {
  uint32_t phase;
  uint32_t reserved;
  uint64_t exfat_size;
  /* The exFAT stage is resumable only at a fully synchronized file boundary.
   * These values identify the exact immutable, sorted source tree captured by
   * the staging image and the number of ordinary files it contains. */
  uint64_t exfat_next_file;
  uint64_t exfat_file_count;
  uint64_t exfat_source_fingerprint;
  uint64_t exfat_stage_size;
  uint64_t pack_next_block;
  uint64_t pack_stored_size;
  uint64_t verify_next_block;
  char inner_name[NAME_MAX + 16];
  char exfat_path[PATH_MAX];
  char pfs_path[PATH_MAX];
} mkpfs_resume_state_t;

typedef int (*mkpfs_resume_checkpoint_callback)(
  const mkpfs_resume_state_t *state, void *opaque);

int mkpfs_normalize_path(const char *input, char *output, size_t output_size);
int mkpfs_scan_folder(const char *root, mkpfs_scan_result_t *result);

/* Return a conservative upper bound for free space required on the destination
 * filesystem while building a folder conversion.  The estimate includes the
 * full temporary exFAT image and the concurrently-written atomic PFS/PFSC
 * output, so callers can reject an unsafe conversion before it starts. */
int mkpfs_estimate_conversion_workspace(const mkpfs_scan_result_t *scan,
                                        uint64_t *bytes_out);

/* Return the conservative peak workspace for a conversion whose exFAT stage
 * is already durable.  This avoids a second source-tree scan on safe restart
 * while retaining the same PFS/PFSC upper bound. */
int mkpfs_estimate_workspace_from_exfat(uint64_t exfat_size,
                                        uint64_t *bytes_out);

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

/* Continue a conversion through caller-owned, private staging paths.  The
 * final output is atomically renamed only after PFSC verification and PFS
 * metadata publication succeed. */
int mkpfs_convert_folder_resumable(
  const char *source, const char *destination, const char *output_name,
  const mkpfs_native_options_t *options, const atomic_int *cancel_requested,
  mkpfs_progress_callback progress, void *progress_opaque,
  mkpfs_resume_state_t *resume,
  mkpfs_resume_checkpoint_callback checkpoint, void *checkpoint_opaque);

#ifdef __cplusplus
}
#endif
