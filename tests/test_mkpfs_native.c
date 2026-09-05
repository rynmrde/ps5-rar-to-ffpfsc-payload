#define _GNU_SOURCE
#include "../src/mkpfs_native.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int progress_cb(uint64_t done, uint64_t total, const char *phase,
                       const char *current, void *opaque) {
  (void)done; (void)total; (void)phase; (void)current; (void)opaque;
  return 0;
}

int main(void) {
  char path[128];
  mkpfs_scan_result_t result;
  assert(mkpfs_normalize_path("/data/game", path, sizeof(path)) == 0);
  assert(!strcmp(path, "/data/game"));
  assert(mkpfs_normalize_path("/data/../game", path, sizeof(path)) != 0);
  assert(mkpfs_normalize_path("relative", path, sizeof(path)) != 0);
  assert(mkpfs_scan_folder("/definitely/not/a/real/folder", &result) != 0);

  const char *input = "/tmp/mkpfs-native-input.bin";
  const char *output = "/tmp/mkpfs-native-output.ffpfsc";
  FILE *fp = fopen(input, "wb"); assert(fp != NULL);
  for (int i = 0; i < 200000; i++) fputc((i * 17) & 0xff, fp);
  fclose(fp);
  assert(mkpfs_pack_pfsc_file(input, output, 6, NULL, progress_cb, NULL) == 0);
  uint64_t logical = 0, blocks = 0;
  assert(mkpfs_verify_pfsc_file(output, &logical, &blocks) == 0);
  assert(logical == 0x40000 && blocks == 4);

  FILE *bad = fopen(output, "r+b"); assert(bad != NULL);
  fseek(bad, 0, SEEK_SET); fputc(0, bad); fclose(bad);
  assert(mkpfs_verify_pfsc_file(output, NULL, NULL) != 0);
  unlink(output); unlink(input);
  puts("mkpfs-native tests passed");
  return 0;
}
