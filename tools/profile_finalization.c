#include "../src/mkpfs_native.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
time_seconds(const struct timespec *start, const struct timespec *end) {
  return (double)(end->tv_sec - start->tv_sec) +
         (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

typedef struct profile_context {
  struct timespec started;
  struct timespec first_compress;
  struct timespec compression_complete;
  int saw_compress;
  int saw_compression_complete;
} profile_context_t;

static int
profile_progress(uint64_t done, uint64_t total, const char *phase,
                 const char *current, void *opaque) {
  profile_context_t *ctx = opaque;

  (void)current;
  if (phase && !strcmp(phase, "compress")) {
    if (!ctx->saw_compress) {
      clock_gettime(CLOCK_MONOTONIC, &ctx->first_compress);
      ctx->saw_compress = 1;
    }
    if (total && done >= total && !ctx->saw_compression_complete) {
      clock_gettime(CLOCK_MONOTONIC, &ctx->compression_complete);
      ctx->saw_compression_complete = 1;
    }
  }
  return 0;
}

int
main(int argc, char **argv) {
  mkpfs_native_options_t options = {0};
  profile_context_t profile = {0};
  struct timespec finished;
  unsigned long workers;
  int rc;

  if (argc != 5) {
    fprintf(stderr, "usage: %s SOURCE_DIR DEST_DIR OUTPUT.ffpfsc WORKERS\n", argv[0]);
    return 2;
  }
  workers = strtoul(argv[4], NULL, 10);
  if (workers > 8) {
    fprintf(stderr, "WORKERS must be between 0 (auto) and 8\n");
    return 2;
  }

  options.compression_level = 7;
  options.workers = (uint32_t)workers;
  options.compression = 1;
  options.verify = 1;
  options.verify_structure = 1;
  clock_gettime(CLOCK_MONOTONIC, &profile.started);
  rc = mkpfs_convert_folder_progress(argv[1], argv[2], argv[3], &options,
                                     NULL, profile_progress, &profile);
  clock_gettime(CLOCK_MONOTONIC, &finished);
  if (rc) {
    errno = rc;
    perror("mkpfs_convert_folder_progress");
    return 1;
  }
  if (!profile.saw_compress || !profile.saw_compression_complete) {
    fprintf(stderr, "conversion did not report a complete compression phase\n");
    return 1;
  }

  printf("exfat_and_compression_start_seconds=%.6f\n",
         time_seconds(&profile.started, &profile.first_compress));
  printf("finalizing_seconds=%.6f\n",
         time_seconds(&profile.compression_complete, &finished));
  printf("total_seconds=%.6f\n", time_seconds(&profile.started, &finished));
  return 0;
}
