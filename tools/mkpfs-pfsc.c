#include "../src/mkpfs_native.h"
#include <stdio.h>
#include <stdlib.h>

static int report(uint64_t done, uint64_t total, const char *phase,
                  const char *current, void *opaque) {
  (void)opaque;
  fprintf(stderr, "\r%s: %llu/%llu bytes (%s)", phase,
          (unsigned long long)done, (unsigned long long)total, current);
  if (done >= total) fputc('\n', stderr);
  return 0;
}

int main(int argc, char **argv) {
  int level = 7;
  unsigned int workers = 1;
  uint64_t logical = 0, blocks = 0;
  if (argc < 3 || argc > 5) {
    fprintf(stderr, "usage: %s INPUT-PFS OUTPUT.ffpfsc [zlib-level] [workers]\n", argv[0]);
    return 2;
  }
  if (argc >= 4) level = atoi(argv[3]);
  if (argc == 5) workers = (unsigned int)strtoul(argv[4], NULL, 10);
  if (mkpfs_pack_pfsc_file_ex(argv[1], argv[2], level, workers, NULL, report, NULL) != 0) {
    perror("mkpfs_pack_pfsc_file_ex"); return 1;
  }
  if (mkpfs_verify_pfsc_file(argv[2], &logical, &blocks) != 0) {
    fprintf(stderr, "verification failed\n"); return 1;
  }
  printf("verified PFSC: logical_size=%llu blocks=%llu workers=%u\n",
         (unsigned long long)logical, (unsigned long long)blocks, workers);
  return 0;
}
