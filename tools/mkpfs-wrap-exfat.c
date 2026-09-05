#include "../src/mkpfs_native.h"
#include <stdio.h>
#include <stdlib.h>

static int progress(uint64_t done, uint64_t total, const char *phase,
                    const char *current, void *opaque) {
  (void)opaque;
  fprintf(stderr, "\r%s: %llu/%llu bytes (%s)", phase,
          (unsigned long long)done, (unsigned long long)total, current);
  if (done >= total) fputc('\n', stderr);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s INPUT.exfat OUTPUT.ffpfsc INNER_NAME\n", argv[0]);
    return 2;
  }
  if (mkpfs_wrap_exfat_file(argv[1], argv[2], argv[3], 7, NULL, progress, NULL) != 0) {
    perror("mkpfs_wrap_exfat_file"); return 1;
  }
  printf("wrote %s\n", argv[2]);
  return 0;
}
