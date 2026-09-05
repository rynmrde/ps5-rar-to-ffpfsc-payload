#include "../src/mkpfs_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int main(int argc, char **argv) {
  mkpfs_native_options_t options = {0};
  if (argc < 4 || argc > 5) {
    fprintf(stderr, "usage: %s SOURCE_DIR DEST_DIR OUTPUT.ffpfsc [WORKERS|auto]\n", argv[0]);
    return 2;
  }
  options.compression_level = 7;
  options.workers = argc == 5 && strcasecmp(argv[4], "auto") ? (unsigned int)strtoul(argv[4], NULL, 10) : 0;
  options.compression = 1;
  options.verify = 1;
  int rc = mkpfs_convert_folder(argv[1], argv[2], argv[3], &options, NULL);
  if (rc) { fprintf(stderr, "conversion failed: %d\n", rc); return 1; }
  printf("wrote %s/%s (workers=%u)\n", argv[2], argv[3], options.workers);
  return 0;
}
