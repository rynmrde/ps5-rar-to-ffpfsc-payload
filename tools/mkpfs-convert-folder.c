#include "../src/mkpfs_native.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  mkpfs_native_options_t options = {0};
  if (argc != 4) {
    fprintf(stderr, "usage: %s SOURCE_DIR DEST_DIR OUTPUT.ffpfsc\n", argv[0]);
    return 2;
  }
  options.compression_level = 7;
  options.compression = 1;
  options.verify = 1;
  int rc = mkpfs_convert_folder(argv[1], argv[2], argv[3], &options, NULL);
  if (rc) { fprintf(stderr, "conversion failed: %d\n", rc); return 1; }
  printf("wrote %s/%s\n", argv[2], argv[3]);
  return 0;
}
