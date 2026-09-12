#include "app_installer.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "asset.h"
#include "notify.h"
#include "pkg_installer.h"

#define INCASSET(name, file)                    \
  __asm__(".section .rodata\n"                  \
          ".global " #name "\n"                 \
          ".global " #name "_end\n"             \
          ".global " #name "_size\n"            \
          ".align 16\n"                         \
          #name ":\n"                           \
          ".incbin \"" file "\"\n"              \
          #name "_end:\n"                       \
          #name "_size:\n"                      \
          ".quad " #name "_end - " #name "\n"   \
          ".previous\n");                       \
  extern const uint8_t name[];                   \
  extern const size_t name##_size;

INCASSET(icon0_png, "assets/icon0.png");

int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

void
app_register_assets(void) {
  static int registered;

  if(!registered) {
    asset_register("/icon0.png", icon0_png, icon0_png_size, "image/png", 0);
    registered = 1;
  }
}

static int sync_parent_directory(const char *path);

static int
install_file(const char *path, const uint8_t *data, size_t size) {
  struct stat st;
  FILE *f;
  int fd;
  if(!stat(path, &st)) {
    return 0;
  }
  if(errno != ENOENT) {
    return -1;
  }
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if(fd < 0 || !(f = fdopen(fd, "wb"))) {
    /* O_EXCL already created an empty final file; remove it so a later
     * stat() cannot mistake it for a completed install. */
    if(fd >= 0) {
      close(fd);
      unlink(path);
    }
    return -1;
  }
  if(fwrite(data, size, 1, f) != 1) {
    int error = errno ? errno : EIO;
    fclose(f);
    unlink(path);
    errno = error;
    return -1;
  }
  /* fclose() must run exactly once: chaining it inside a short-circuit ||
   * expression would skip the close when fflush()/fsync() fails and leak
   * the FILE * plus its descriptor. */
  {
    int install_error = 0;
    if(fflush(f)) {
      install_error = errno ? errno : EIO;
    } else if(fsync(fileno(f))) {
      install_error = errno ? errno : EIO;
    }
    if(fclose(f) && !install_error) {
      install_error = errno ? errno : EIO;
    }
    if(install_error) {
      unlink(path);
      errno = install_error;
      return -1;
    }
  }
  if(sync_parent_directory(path)) return -1;
  return 1;
}

/* AppInstUtil can inspect the title directory from a separate service thread.
 * Make an atomic metadata/icon replacement visible and durable before asking
 * that service to re-register the title. */
static int
sync_parent_directory(const char *path) {
  char parent[PATH_MAX];
  char *slash;
  int fd;
  int error;

  if(strlen(path) >= sizeof(parent)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(parent, path);
  slash = strrchr(parent, '/');
  if(!slash || slash == parent) {
    errno = EINVAL;
    return -1;
  }
  *slash = 0;
  fd = open(parent, O_RDONLY | O_DIRECTORY);
  if(fd < 0) return -1;
  if(fsync(fd)) {
    error = errno ? errno : EIO;
    close(fd);
    errno = error;
    return -1;
  }
  return close(fd);
}

static int
install_app(const char *title_id, const char *dir) {
  int (*sceAppInstUtilAppInstallTitleDir)(const char *, const char *, void *) = 0;
  const char *nid = "Wudg3Xe3heE";
  uint32_t handle;

  if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    sceAppInstUtilAppInstallTitleDir =
      (void *)kernel_dynlib_resolve(-1, handle, nid);
  }

  if(sceAppInstUtilAppInstallTitleDir) {
    return sceAppInstUtilAppInstallTitleDir(title_id, dir, 0);
  }

  return sceAppInstUtilAppInstallAll(0);
}

static int
write_owned_param(const char *path, const char *data, size_t size) {
  char current[1024];
  char tmp[PATH_MAX];
  FILE *f = fopen(path, "rb");
  int fd;
  size_t n;

  if(!f) {
    return errno == ENOENT ? install_file(path, (const uint8_t *)data, size) : -1;
  }
  n = fread(current, 1, sizeof(current) - 1, f);
  fclose(f);
  current[n] = 0;
  if(!strstr(current, "\"titleId\": \"FMGR88888\"")) {
    errno = EEXIST;
    return -1;
  }
  if(n == size && !memcmp(current, data, size)) return 0;
  snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
  fd = mkstemp(tmp);
  if(fd < 0) return -1;
  f = fdopen(fd, "wb");
  if(!f) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  if(fwrite(data, size, 1, f) != 1) {
    int error = errno ? errno : EIO;
    fclose(f);
    unlink(tmp);
    errno = error;
    return -1;
  }
  {
    int install_error = 0;
    if(fflush(f)) {
      install_error = errno ? errno : EIO;
    } else if(fsync(fileno(f))) {
      install_error = errno ? errno : EIO;
    }
    if(fclose(f) && !install_error) {
      install_error = errno ? errno : EIO;
    }
    if(install_error) {
      unlink(tmp);
      errno = install_error;
      return -1;
    }
  }
  if(rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  if(sync_parent_directory(path)) return -1;
  return 1;
}

static int
write_owned_binary(const char *path, const uint8_t *data, size_t size) {
  char tmp[PATH_MAX];
  struct stat st;
  FILE *f;
  int fd;
  int exists;

  exists = lstat(path, &st) == 0;
  if(exists && (!S_ISREG(st.st_mode) || st.st_nlink != 1)) {
    errno = EINVAL;
    return -1;
  }
  if(!exists && errno != ENOENT) return -1;
  if(exists && S_ISREG(st.st_mode) && st.st_size == (off_t)size) {
    FILE *current = fopen(path, "rb");
    int same = 0;
    if(current) {
      uint8_t buffer[4096];
      size_t offset = 0;
      same = 1;
      while(offset < size) {
        size_t chunk = size - offset;
        if(chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if(fread(buffer, 1, chunk, current) != chunk ||
           memcmp(buffer, data + offset, chunk)) {
          same = 0;
          break;
        }
        offset += chunk;
      }
      fclose(current);
    }
    if(same) return 0;
  }
  if(snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = mkstemp(tmp);
  if(fd < 0) return -1;
  f = fdopen(fd, "wb");
  if(!f) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  {
    int install_error = 0;
    if(fwrite(data, size, 1, f) != 1) {
      install_error = errno ? errno : EIO;
    } else if(fflush(f)) {
      install_error = errno ? errno : EIO;
    } else if(fsync(fileno(f))) {
      install_error = errno ? errno : EIO;
    }
    if(fclose(f) && !install_error) {
      install_error = errno ? errno : EIO;
    }
    if(!install_error && rename(tmp, path)) {
      install_error = errno ? errno : EIO;
    }
    if(install_error) {
      unlink(tmp);
      errno = install_error;
      return -1;
    }
  }
  if(sync_parent_directory(path)) return -1;
  return 1;
}

/* Transactional launcher refresh helpers.
 *
 * The firmware registry transaction is uninstall-then-install, which is not
 * atomic: if the install half fails, the title is left unregistered.  The
 * strategy below narrows that window as far as userland can:
 *
 *  1. Snapshot the current metadata before overwriting anything, so a
 *     failed refresh can roll the staged files back and re-register the
 *     previous known-good content.
 *  2. Stage the replacement files with temp-file + fsync + atomic rename,
 *     then read them back and verify byte equality before touching the
 *     registry at all.
 *  3. Record a pending-registration marker outside the title directory so
 *     a crash or power loss between uninstall and install is retried on
 *     the next payload start instead of being mistaken for success.
 *  4. Retry the install half with a bounded backoff, re-verifying staging
 *     before each attempt, and report every outcome both to stderr and to
 *     the on-screen notification queue.
 */

#define LAUNCHER_PARAM_BACKUP_MAX (4096u)
#define LAUNCHER_ICON_BACKUP_MAX (4u * 1024u * 1024u)
#define LAUNCHER_INSTALL_ATTEMPTS 3

/* Reads a regular file into a freshly allocated buffer.  Returns 0 with
 * *data_out set on success, 1 when the file does not exist (first install),
 * and -1 on any other error.  Symlinks, hardlinks, and oversized files are
 * refused so a tampered title directory fails closed. */
static int
read_backup_file(const char *path, void **data_out, size_t *size_out,
                 size_t max_size) {
  struct stat st;
  FILE *f;
  void *buffer;
  size_t got;
  int close_error;

  *data_out = NULL;
  *size_out = 0;
  if(lstat(path, &st)) {
    return errno == ENOENT ? 1 : -1;
  }
  if(!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
     (uintmax_t)st.st_size > (uintmax_t)max_size) {
    errno = EINVAL;
    return -1;
  }
  f = fopen(path, "rb");
  if(!f) return -1;
  buffer = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
  if(!buffer) {
    fclose(f);
    return -1;
  }
  got = fread(buffer, 1, (size_t)st.st_size, f);
  close_error = fclose(f);
  if(got != (size_t)st.st_size || close_error) {
    free(buffer);
    errno = EIO;
    return -1;
  }
  *data_out = buffer;
  *size_out = (size_t)st.st_size;
  return 0;
}

/* Proves the staged file on disk is exactly the expected content.  The
 * lstat() gate refuses symlinks and hardlinks; the size check runs before
 * any byte is read so a truncated stage cannot compare equal. */
static int
verify_launcher_file(const char *path, const void *expected,
                     size_t expected_size) {
  struct stat st;
  FILE *f;
  uint8_t buffer[4096];
  size_t offset = 0;

  if(!path || (!expected && expected_size)) {
    errno = EINVAL;
    return -1;
  }
  if(lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
     st.st_size < 0 || (size_t)st.st_size != expected_size) {
    return -1;
  }
  f = fopen(path, "rb");
  if(!f) return -1;
  while(offset < expected_size) {
    size_t chunk = expected_size - offset;
    size_t got;
    if(chunk > sizeof(buffer)) chunk = sizeof(buffer);
    got = fread(buffer, 1, chunk, f);
    if(got != chunk ||
       memcmp(buffer, (const uint8_t *)expected + offset, chunk)) {
      fclose(f);
      errno = EIO;
      return -1;
    }
    offset += chunk;
  }
  if(fclose(f)) return -1;
  return 0;
}

/* Best-effort rollback writer used only after a failed refresh.  It stages
 * through a temp file and atomic rename like the primary writers. */
static int
restore_launcher_file(const char *path, const void *data, size_t size) {
  char tmp[PATH_MAX];
  FILE *f;
  int fd;
  int restore_error = 0;

  if(snprintf(tmp, sizeof(tmp), "%s.restore.XXXXXX", path) >= (int)sizeof(tmp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = mkstemp(tmp);
  if(fd < 0) return -1;
  f = fdopen(fd, "wb");
  if(!f) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  if(size && fwrite(data, size, 1, f) != 1) {
    restore_error = errno ? errno : EIO;
  } else if(fflush(f)) {
    restore_error = errno ? errno : EIO;
  } else if(fsync(fileno(f))) {
    restore_error = errno ? errno : EIO;
  }
  if(fclose(f) && !restore_error) {
    restore_error = errno ? errno : EIO;
  }
  if(!restore_error && rename(tmp, path)) {
    restore_error = errno ? errno : EIO;
  }
  if(restore_error) {
    unlink(tmp);
    errno = restore_error;
    return -1;
  }
  if(sync_parent_directory(path)) return -1;
  return 0;
}

static int
write_install_pending(const char *pending_path, unsigned short port) {
  char tmp[PATH_MAX];
  char payload[32];
  int length;
  int fd;

  length = snprintf(payload, sizeof(payload), "port=%u\n", (unsigned int)port);
  if(length < 0 || (size_t)length >= sizeof(payload)) {
    errno = EINVAL;
    return -1;
  }
  if(snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", pending_path) >=
     (int)sizeof(tmp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = mkstemp(tmp);
  if(fd < 0) return -1;
  {
    size_t done = 0;
    while(done < (size_t)length) {
      ssize_t wrote = write(fd, payload + done, (size_t)length - done);
      if(wrote < 0) {
        if(errno == EINTR) continue;
        close(fd);
        unlink(tmp);
        return -1;
      }
      if(!wrote) {
        close(fd);
        unlink(tmp);
        errno = EIO;
        return -1;
      }
      done += (size_t)wrote;
    }
  }
  /* close() must run even when fsync() fails: a short-circuit || chain would
   * skip it and leak the descriptor. */
  {
    int pending_error = 0;
    if(fsync(fd)) pending_error = errno ? errno : EIO;
    if(close(fd) && !pending_error) pending_error = errno ? errno : EIO;
    if(pending_error) {
      unlink(tmp);
      errno = pending_error;
      return -1;
    }
  }
  if(rename(tmp, pending_path)) {
    int error = errno ? errno : EIO;
    unlink(tmp);
    errno = error;
    return -1;
  }
  return sync_parent_directory(pending_path);
}

static void
clear_install_pending(const char *pending_path) {
  if(unlink(pending_path) && errno != ENOENT) {
    fprintf(stderr, "clear install pending marker failed: %s\n",
            strerror(errno));
    return;
  }
  /* A missing fsync here only risks a redundant refresh on the next boot
   * after a crash, never an unregistered title. */
  (void)sync_parent_directory(pending_path);
}

int
app_install_if_needed(unsigned short port) {
  const char *title_id = TITLE_ID;
  char base_dir[256];
  char sce_sys_dir[256];
  char param_path[256];
  char icon_path[256];
  char pending_path[256];
  struct stat pending_st;
  void *backup_param = NULL;
  void *backup_icon = NULL;
  size_t backup_param_size = 0;
  size_t backup_icon_size = 0;
  int have_param_backup = 0;
  int have_icon_backup = 0;
  int pending_exists = 0;
  int attempt;
  int uninstall_rc;
  int err;
  char param_json[512];

  app_register_assets();

  snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "/user/app/%s/sce_sys", title_id);
  snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);
  /* The pending marker lives beside the title directory, never inside
   * sce_sys, so firmware metadata validation cannot trip over it. */
  snprintf(pending_path, sizeof(pending_path), "/user/app/.mkpfs-%s-pending",
           title_id);

  snprintf(param_json, sizeof(param_json),
           "{\n  \"applicationCategoryType\": 65536,\n"
           "  \"titleId\": \"%s\",\n"
           "  \"deeplinkUri\": \"http://127.0.0.1:%u/\",\n"
           "  \"localizedParameters\": {\n"
           "    \"defaultLanguage\": \"en-US\",\n"
           "    \"en-US\": {\"titleName\": \"MkPFS-PS5\"},\n"
           "    \"zh-Hans\": {\"titleName\": \"MkPFS-PS5\"},\n"
           "    \"zh-Hant\": {\"titleName\": \"MkPFS-PS5\"}\n"
           "  }\n}\n", title_id, (unsigned int)port);

  printf("Installing or refreshing launcher app %s on port %u\n",
         title_id, (unsigned int)port);

  if((err = pkg_installer_initialize())) {
    printf("sceAppInstUtilInitialize: error 0x%08X\n", err);
    return -1;
  }
  if(mkdir(base_dir, 0755) && errno != EEXIST) {
    perror("mkdir app dir");
    return -1;
  }
  if(mkdir(sce_sys_dir, 0755) && errno != EEXIST) {
    perror("mkdir sce_sys dir");
    return -1;
  }
  /* Snapshot the current metadata before overwriting anything.  A failed
   * refresh below can then roll the staged files back and re-register the
   * previous known-good content instead of leaving nothing registered. */
  {
    int backup_rc = read_backup_file(param_path, &backup_param,
                                     &backup_param_size,
                                     LAUNCHER_PARAM_BACKUP_MAX);
    if(backup_rc < 0) {
      perror("backup launcher param");
      return -1;
    }
    have_param_backup = backup_rc == 0;
  }
  {
    int backup_rc = read_backup_file(icon_path, &backup_icon,
                                     &backup_icon_size,
                                     LAUNCHER_ICON_BACKUP_MAX);
    if(backup_rc < 0) {
      perror("backup launcher icon");
      free(backup_param);
      return -1;
    }
    have_icon_backup = backup_rc == 0;
  }
  {
    int param_changed = write_owned_param(param_path, param_json, strlen(param_json));
    int icon_changed = write_owned_binary(icon_path, icon0_png, icon0_png_size);
    if(param_changed < 0 || icon_changed < 0) {
      perror("install launcher assets");
      notify_user("MkPFS launcher staging failed; keeping the current Home Screen entry");
      free(backup_param);
      free(backup_icon);
      return -1;
    }
    pending_exists = !lstat(pending_path, &pending_st);
    if(!param_changed && !icon_changed && !pending_exists) {
      printf("Launcher app %s is already up to date on port %u\n",
             title_id, (unsigned int)port);
      free(backup_param);
      free(backup_icon);
      return 0;
    }
  }
  /* The replacement files are staged.  Read them back and prove byte
   * equality before the registry is touched: a transient storage failure
   * must turn into a kept old title, never a missing launcher. */
  if(verify_launcher_file(param_path, param_json, strlen(param_json)) ||
     verify_launcher_file(icon_path, icon0_png, icon0_png_size)) {
    fprintf(stderr, "launcher staging verification failed; keeping registered title\n");
    notify_user("MkPFS launcher verification failed; keeping the current Home Screen entry");
    if(have_param_backup) {
      (void)restore_launcher_file(param_path, backup_param, backup_param_size);
    }
    if(have_icon_backup) {
      (void)restore_launcher_file(icon_path, backup_icon, backup_icon_size);
    }
    free(backup_param);
    free(backup_icon);
    return -1;
  }
  /* A crash between uninstall and install must be retried on the next
   * payload start rather than mistaken for an up-to-date launcher. */
  if(write_install_pending(pending_path, port)) {
    perror("write install pending marker");
    fprintf(stderr, "continuing without a pending marker; a crash before "
            "registration completes will need a manual refresh\n");
  }
  /* AppInstallAll does not reliably refresh an already registered title on
   * every PS5 firmware, so the old registration is removed first.  Only
   * this payload's own title is ever removed, and only after the
   * replacement directory has been completely written, synced, and
   * verified above.  A missing previous registration is expected on a
   * first install and is not fatal. */
  uninstall_rc = sceAppInstUtilAppUnInstall(title_id);
  if(uninstall_rc) {
    printf("sceAppInstUtilAppUnInstall: 0x%08X (continuing)\n",
           (unsigned int)uninstall_rc);
  }
  err = 0;
  for(attempt = 0; attempt < LAUNCHER_INSTALL_ATTEMPTS; attempt++) {
    err = install_app(title_id, "/user/app/");
    if(!err) break;
    fprintf(stderr, "install_app attempt %d/%d failed: 0x%08X\n",
            attempt + 1, LAUNCHER_INSTALL_ATTEMPTS, (unsigned int)err);
    if(attempt + 1 < LAUNCHER_INSTALL_ATTEMPTS) {
      usleep(250000);
      /* Storage may have dropped the staging between attempts; never
       * retry the registry half against unverified files. */
      if(verify_launcher_file(param_path, param_json, strlen(param_json)) ||
         verify_launcher_file(icon_path, icon0_png, icon0_png_size)) {
        fprintf(stderr, "launcher staging lost before retry; aborting\n");
        break;
      }
    }
  }
  if(err) {
    /* Last resort: roll the staged files back to the previous content and
     * re-register that, so the console keeps a working title instead of
     * none.  On a first install there is no backup; the verified staged
     * files stay in place with the pending marker so the next payload
     * start retries the install half. */
    if(have_param_backup || have_icon_backup) {
      int restored = 1;
      if(have_param_backup &&
         restore_launcher_file(param_path, backup_param, backup_param_size)) {
        perror("restore launcher param");
        restored = 0;
      }
      if(have_icon_backup &&
         restore_launcher_file(icon_path, backup_icon, backup_icon_size)) {
        perror("restore launcher icon");
        restored = 0;
      }
      if(restored) {
        int fallback = install_app(title_id, "/user/app/");
        if(!fallback) {
          fprintf(stderr, "launcher refresh failed (0x%08X); previous "
                  "registration restored\n", (unsigned int)err);
          notify_user("MkPFS launcher update failed (0x%08X); previous version restored",
                      (unsigned int)err);
          clear_install_pending(pending_path);
          free(backup_param);
          free(backup_icon);
          return -1;
        }
        err = fallback;
      }
    }
    fprintf(stderr, "install_app failed after %d attempts: 0x%08X; staged "
            "files kept for retry on next launch\n", LAUNCHER_INSTALL_ATTEMPTS,
            (unsigned int)err);
    notify_user("MkPFS launcher refresh failed (0x%08X); will retry on next launch. Web server: port %u",
                (unsigned int)err, (unsigned int)port);
    free(backup_param);
    free(backup_icon);
    return -1;
  }
  if(verify_launcher_file(param_path, param_json, strlen(param_json)) ||
     verify_launcher_file(icon_path, icon0_png, icon0_png_size)) {
    fprintf(stderr, "launcher post-install verification failed; "
            "will retry on next launch\n");
    notify_user("MkPFS launcher post-install check failed; will retry on next launch");
    free(backup_param);
    free(backup_icon);
    return -1;
  }
  clear_install_pending(pending_path);
  printf("Launcher app %s installed/refreshed on port %u\n",
         title_id, (unsigned int)port);
  free(backup_param);
  free(backup_icon);
  return 0;
}
