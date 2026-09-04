#include "../src/mkpfs_native.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  char path[128];
  mkpfs_scan_result_t result;
  assert(mkpfs_normalize_path("/data/game", path, sizeof(path)) == 0);
  assert(!strcmp(path, "/data/game"));
  assert(mkpfs_normalize_path("/data/../game", path, sizeof(path)) != 0);
  assert(mkpfs_normalize_path("relative", path, sizeof(path)) != 0);
  assert(mkpfs_scan_folder("/definitely/not/a/real/folder", &result) != 0);
  assert(mkpfs_convert_folder("/tmp", "/tmp", "x.ffpfsc", NULL, NULL) == ENOTSUP);
  puts("mkpfs-native tests passed");
  return 0;
}
