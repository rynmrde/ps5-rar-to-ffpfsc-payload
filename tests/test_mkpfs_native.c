#include "../src/mkpfs_native.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static int progress_cb(uint64_t done, uint64_t total, const char *phase,
                       const char *current, void *opaque) {
  (void)done; (void)total; (void)phase; (void)current; (void)opaque;
  return 0;
}

static int cancel_on_verify_cb(uint64_t done, uint64_t total, const char *phase,
                               const char *current, void *opaque) {
  (void)done; (void)total; (void)current; (void)opaque;
  return phase && !strcmp(phase, "verify");
}

static int same_file(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
  unsigned char x[4096], y[4096]; size_t nx, ny;
  if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
  do { nx = fread(x, 1, sizeof(x), fa); ny = fread(y, 1, sizeof(y), fb); if (nx != ny || memcmp(x, y, nx)) { fclose(fa); fclose(fb); return 0; } } while (nx);
  fclose(fa); fclose(fb); return 1;
}

int main(void) {
  char path[128]; mkpfs_scan_result_t result;
  assert(mkpfs_normalize_path("/data/game", path, sizeof(path)) == 0);
  assert(!strcmp(path, "/data/game"));
  assert(mkpfs_normalize_path("/data/../game", path, sizeof(path)) != 0);
  assert(mkpfs_normalize_path("relative", path, sizeof(path)) != 0);
  assert(mkpfs_scan_folder("/definitely/not/a/real/folder", &result) != 0);

  const char *input = "/tmp/mkpfs-native-input.bin";
  const char *output = "/tmp/mkpfs-native-output.ffpfsc";
  const char *parallel = "/tmp/mkpfs-native-output-parallel.ffpfsc";
  const char *canceled = "/tmp/mkpfs-native-canceled.ffpfsc";
  const char *wrapped = "/tmp/mkpfs-native-wrapped.ffpfsc";
  const char *verify_canceled = "/tmp/mkpfs-native-verify-canceled.ffpfsc";
  FILE *fp = fopen(input, "wb"); assert(fp != NULL);
  for (int i = 0; i < 200000; i++) fputc((i * 17) & 0xff, fp);
  fclose(fp);
  assert(mkpfs_pack_pfsc_file(input, output, 6, NULL, progress_cb, NULL) == 0);
  assert(mkpfs_pack_pfsc_file_ex(input, parallel, 6, 4, NULL, progress_cb, NULL) == 0);
  assert(same_file(output, parallel));
  uint64_t logical = 0, blocks = 0;
  assert(mkpfs_verify_pfsc_file(output, &logical, &blocks) == 0);
  assert(logical == 0x40000 && blocks == 4);
  assert(mkpfs_wrap_exfat_file(input, wrapped, "test.exfat", 6, NULL,
                               progress_cb, NULL) == 0);
  assert(access(wrapped, F_OK) == 0);
  assert(mkpfs_wrap_exfat_file(input, verify_canceled, "test.exfat", 6, NULL,
                               cancel_on_verify_cb, NULL) == ECANCELED);
  assert(access(verify_canceled, F_OK) != 0);

  atomic_int cancel = 1;
  assert(mkpfs_pack_pfsc_file_ex(input, canceled, 6, 4, &cancel, progress_cb, NULL) == ECANCELED);
  assert(access(canceled, F_OK) != 0);

  FILE *bad = fopen(output, "r+b"); assert(bad != NULL);
  fseek(bad, 0, SEEK_SET); fputc(0, bad); fclose(bad);
  assert(mkpfs_verify_pfsc_file(output, NULL, NULL) != 0);
  unlink(output); unlink(parallel); unlink(canceled); unlink(wrapped);
  unlink(verify_canceled); unlink(input);
  puts("mkpfs-native tests passed");
  return 0;
}
