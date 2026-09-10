#include "../src/mkpfs_native.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct checkpoint_context {
  mkpfs_resume_state_t latest;
  uint32_t interrupt_phase;
  int interrupted;
} checkpoint_context_t;

static int
progress_cb(uint64_t done, uint64_t total, const char *phase,
            const char *current, void *opaque) {
  (void)done;
  (void)total;
  (void)phase;
  (void)current;
  (void)opaque;
  return 0;
}

static int
checkpoint_cb(const mkpfs_resume_state_t *state, void *opaque) {
  checkpoint_context_t *context = opaque;

  context->latest = *state;
  if(!context->interrupted && state->phase == context->interrupt_phase &&
     ((state->phase == MKPFS_RESUME_EXFAT && state->exfat_next_file) ||
      (state->phase == MKPFS_RESUME_PACK && state->pack_next_block) ||
      (state->phase == MKPFS_RESUME_VERIFY && state->verify_next_block) ||
      (state->phase == MKPFS_RESUME_PUBLISH && state->pack_next_block))) {
    context->interrupted = 1;
    return -1;
  }
  return 0;
}

static int
same_file(const char *left, const char *right) {
  unsigned char a[8192];
  unsigned char b[8192];
  FILE *fa = fopen(left, "rb");
  FILE *fb = fopen(right, "rb");
  size_t na;
  size_t nb;

  if(!fa || !fb) {
    if(fa) fclose(fa);
    if(fb) fclose(fb);
    return 0;
  }
  do {
    na = fread(a, 1, sizeof(a), fa);
    nb = fread(b, 1, sizeof(b), fb);
    if(na != nb || memcmp(a, b, na)) {
      fclose(fa);
      fclose(fb);
      return 0;
    }
  } while(na);
  fclose(fa);
  fclose(fb);
  return 1;
}

static uint64_t
stage_hash(const char *value) {
  uint64_t hash = UINT64_C(1469598103934665603);

  for(; *value; value++) {
    hash ^= (unsigned char)*value;
    hash *= UINT64_C(1099511628211);
  }
  hash ^= 0;
  return hash * UINT64_C(1099511628211);
}

static void
write_fixture(const char *path) {
  unsigned char buffer[65536];
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

  assert(fd >= 0);
  for(size_t i = 0; i < sizeof(buffer); i++) {
    buffer[i] = (unsigned char)((i * 29u + 17u) & 0xffu);
  }
  for(int i = 0; i < 224; i++) {
    assert(write(fd, buffer, sizeof(buffer)) == (ssize_t)sizeof(buffer));
  }
  assert(close(fd) == 0);
}

static void
write_sparse_fixture(const char *path, off_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

  assert(fd >= 0);
  assert(ftruncate(fd, size) == 0);
  assert(close(fd) == 0);
}

static void
run_resume_case(const char *source, const char *destination,
                const char *output, uint32_t interrupt_phase,
                int hide_source_before_resume, int modify_source_before_resume) {
  mkpfs_native_options_t options = {0};
  mkpfs_resume_state_t state = {0};
  checkpoint_context_t interrupted = {0};
  checkpoint_context_t continued = {0};
  char stage_exfat[PATH_MAX];
  char stage_pfs[PATH_MAX];
  char final_path[PATH_MAX];
  char offline_source[PATH_MAX];
  uint64_t hash;
  int conversion_result;

  options.compression_level = 7;
  options.workers = 1;
  options.compression = 1;
  options.verify = 1;
  options.verify_structure = 1;
  state.phase = MKPFS_RESUME_EXFAT;
  assert(snprintf(final_path, sizeof(final_path), "%s/%s", destination,
                  output) < (int)sizeof(final_path));
  hash = stage_hash(final_path);
  assert(snprintf(stage_exfat, sizeof(stage_exfat),
                  "%s/.%s.mkpfs-%016llx.exfat.stage", destination, output,
                  (unsigned long long)hash) < (int)sizeof(stage_exfat));
  assert(snprintf(stage_pfs, sizeof(stage_pfs),
                  "%s/.%s.mkpfs-%016llx.pfs.stage", destination, output,
                  (unsigned long long)hash) < (int)sizeof(stage_pfs));
  snprintf(state.exfat_path, sizeof(state.exfat_path), "%s", stage_exfat);
  snprintf(state.pfs_path, sizeof(state.pfs_path), "%s", stage_pfs);
  interrupted.interrupt_phase = interrupt_phase;

  conversion_result = mkpfs_convert_folder_resumable(
    source, destination, output, &options, NULL, progress_cb, NULL, &state,
    checkpoint_cb, &interrupted);
  assert(conversion_result == EIO);
  assert(interrupted.interrupted);
  assert(access(final_path, F_OK) != 0);
  if(interrupt_phase == MKPFS_RESUME_EXFAT) {
    assert(access(stage_exfat, F_OK) == 0);
  } else {
    assert(access(stage_pfs, F_OK) == 0);
  }

  if(hide_source_before_resume) {
    assert(snprintf(offline_source, sizeof(offline_source), "%s.offline", source) <
           (int)sizeof(offline_source));
    assert(rename(source, offline_source) == 0);
  }

  if(modify_source_before_resume) {
    int fd;

    assert(interrupt_phase == MKPFS_RESUME_EXFAT);
    assert(snprintf(offline_source, sizeof(offline_source), "%s/0-large.bin",
                    source) < (int)sizeof(offline_source));
    fd = open(offline_source, O_WRONLY | O_APPEND);
    assert(fd >= 0);
    assert(write(fd, "X", 1) == 1);
    assert(close(fd) == 0);
  }

  continued.interrupt_phase = 0;
  conversion_result = mkpfs_convert_folder_resumable(
    source, destination, output, &options, NULL, progress_cb, NULL,
    &interrupted.latest, checkpoint_cb, &continued);
  assert(conversion_result == (modify_source_before_resume ? ESTALE : 0));
  if(modify_source_before_resume) {
    assert(access(final_path, F_OK) != 0);
    assert(access(stage_exfat, F_OK) == 0);
    assert(unlink(stage_exfat) == 0);
    assert(access(stage_pfs, F_OK) != 0);
    return;
  }
  if(hide_source_before_resume) {
    assert(rename(offline_source, source) == 0);
  }
  assert(access(final_path, F_OK) == 0);
  assert(access(stage_exfat, F_OK) != 0);
  assert(access(stage_pfs, F_OK) != 0);
}

int
main(void) {
  const char *root = "/tmp/mkpfs-native-resume";
  const char *source = "/tmp/mkpfs-native-resume/source";
  const char *baseline_dir = "/tmp/mkpfs-native-resume/baseline";
  const char *pack_dir = "/tmp/mkpfs-native-resume/pack";
  const char *exfat_dir = "/tmp/mkpfs-native-resume/exfat";
  const char *changed_dir = "/tmp/mkpfs-native-resume/changed";
  const char *verify_dir = "/tmp/mkpfs-native-resume/verify";
  const char *publish_dir = "/tmp/mkpfs-native-resume/publish";
  const char *baseline = "/tmp/mkpfs-native-resume/baseline/resume.ffpfsc";
  const char *pack_output = "/tmp/mkpfs-native-resume/pack/resume.ffpfsc";
  const char *exfat_output = "/tmp/mkpfs-native-resume/exfat/resume.ffpfsc";
  const char *verify_output = "/tmp/mkpfs-native-resume/verify/resume.ffpfsc";
  const char *publish_output = "/tmp/mkpfs-native-resume/publish/resume.ffpfsc";
  char source_file[PATH_MAX];
  char param[PATH_MAX];
  FILE *fp;
  int close_result;
  mkpfs_native_options_t options = {0};

  assert(mkdir(root, 0700) == 0);
  assert(mkdir(source, 0700) == 0);
  assert(mkdir("/tmp/mkpfs-native-resume/source/sce_sys", 0700) == 0);
  assert(mkdir(baseline_dir, 0700) == 0);
  assert(mkdir(pack_dir, 0700) == 0);
  assert(mkdir(exfat_dir, 0700) == 0);
  assert(mkdir(changed_dir, 0700) == 0);
  assert(mkdir(verify_dir, 0700) == 0);
  assert(mkdir(publish_dir, 0700) == 0);
  assert(snprintf(param, sizeof(param), "%s/sce_sys/param.json", source) <
         (int)sizeof(param));
  fp = fopen(param, "wb");
  assert(fp != NULL);
  assert(fputs("{\"titleId\":\"PRESUME01\"}\n", fp) >= 0);
  close_result = fclose(fp);
  assert(close_result == 0);
  assert(snprintf(source_file, sizeof(source_file), "%s/payload.bin", source) <
         (int)sizeof(source_file));
  write_fixture(source_file);
  assert(snprintf(source_file, sizeof(source_file), "%s/0-large.bin", source) <
         (int)sizeof(source_file));
  write_sparse_fixture(source_file, 64 * 1024 * 1024);
  assert(snprintf(source_file, sizeof(source_file), "%s/1-large.bin", source) <
         (int)sizeof(source_file));
  write_sparse_fixture(source_file, 64 * 1024 * 1024);

  options.compression_level = 7;
  options.workers = 1;
  options.compression = 1;
  options.verify = 1;
  options.verify_structure = 1;
  assert(mkpfs_convert_folder_progress(source, baseline_dir, "resume.ffpfsc",
                                       &options, NULL, progress_cb, NULL) == 0);

  run_resume_case(source, exfat_dir, "resume.ffpfsc", MKPFS_RESUME_EXFAT, 0, 0);
  run_resume_case(source, pack_dir, "resume.ffpfsc", MKPFS_RESUME_PACK, 1, 0);
  run_resume_case(source, verify_dir, "resume.ffpfsc", MKPFS_RESUME_VERIFY, 0, 0);
  run_resume_case(source, publish_dir, "resume.ffpfsc", MKPFS_RESUME_PUBLISH, 0, 0);
  assert(same_file(baseline, pack_output));
  assert(same_file(baseline, exfat_output));
  assert(same_file(baseline, verify_output));
  assert(same_file(baseline, publish_output));

  run_resume_case(source, changed_dir, "resume.ffpfsc", MKPFS_RESUME_EXFAT, 0, 1);

  unlink(baseline);
  unlink(pack_output);
  unlink(exfat_output);
  unlink(verify_output);
  unlink(publish_output);
  assert(snprintf(source_file, sizeof(source_file), "%s/0-large.bin", source) <
         (int)sizeof(source_file));
  unlink(source_file);
  assert(snprintf(source_file, sizeof(source_file), "%s/1-large.bin", source) <
         (int)sizeof(source_file));
  unlink(source_file);
  assert(snprintf(source_file, sizeof(source_file), "%s/payload.bin", source) <
         (int)sizeof(source_file));
  unlink(source_file);
  unlink(param);
  rmdir("/tmp/mkpfs-native-resume/source/sce_sys");
  rmdir(source);
  rmdir(baseline_dir);
  rmdir(pack_dir);
  rmdir(exfat_dir);
  rmdir(changed_dir);
  rmdir(verify_dir);
  rmdir(publish_dir);
  rmdir(root);
  puts("mkpfs resumable conversion tests passed");
  return 0;
}
