#define _GNU_SOURCE
#include "../src/mkpfs_native.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  char path[128];
  mkpfs_scan_result_t result;
  assert(mkpfs_normalize_path("/data/game", path, sizeof(path)) == 0);
  assert(!strcmp(path, "/data/game"));
  assert(mkpfs_normalize_path("/data/../game", path, sizeof(path)) != 0);
  assert(mkpfs_normalize_path("relative", path, sizeof(path)) != 0);
  assert(mkpfs_scan_folder("/definitely/not/a/real/folder", &result) != 0);
  char temp[] = "/tmp/mkpfs-native-test";
  rmdir(temp);
  assert(mkdir(temp, 0700) == 0);
  char nested[PATH_MAX];
  snprintf(nested, sizeof(nested), "%s/nested", temp);
  assert(mkdir(nested, 0700) == 0);
  char sample[PATH_MAX];
  strcpy(sample, nested);
  strcat(sample, "/sample.bin");
  FILE *fp = fopen(sample, "wb"); assert(fp != NULL); fputs("sample", fp); fclose(fp);
  assert(mkpfs_scan_folder(temp, &result) == 0);
  assert(result.file_count == 1 && result.directory_count == 1 && result.total_bytes == 6);
  assert(mkpfs_convert_folder(temp, temp, "x.ffpfsc", NULL, NULL) == ENOTSUP);
  unlink(sample); rmdir(nested); rmdir(temp);
  puts("mkpfs-native tests passed");
  return 0;
}
