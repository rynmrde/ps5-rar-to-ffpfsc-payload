#include "../src/mkpfs_native.h"
#include <stdio.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s SOURCE_DIR OUTPUT.exfat\n", argv[0]);
    return 2;
  }
  int rc = mkpfs_build_exfat_folder(argv[1], argv[2], NULL, NULL, NULL);
  if (rc) { fprintf(stderr, "exFAT build failed: %d\n", rc); return 1; }
  printf("wrote %s\n", argv[2]);
  return 0;
}
