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
    if(fd >= 0) close(fd);
    return -1;
  }
  if(fwrite(data, size, 1, f) != 1) {
    fclose(f);
    unlink(path);
    return -1;
  }
  if(fflush(f) || fsync(fileno(f)) || fclose(f)) {
    unlink(path);
    return -1;
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
    fclose(f);
    unlink(tmp);
    return -1;
  }
  if(fflush(f) || fsync(fileno(f)) || fclose(f)) {
    unlink(tmp);
    return -1;
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
  if(fwrite(data, size, 1, f) != 1 || fflush(f) || fsync(fileno(f)) ||
     fclose(f) || rename(tmp, path)) {
    int error = errno ? errno : EIO;
    unlink(tmp);
    errno = error;
    return -1;
  }
  if(sync_parent_directory(path)) return -1;
  return 1;
}

int
app_install_if_needed(unsigned short port) {
  const char *title_id = TITLE_ID;
  char base_dir[256];
  char sce_sys_dir[256];
  char param_path[256];
  char icon_path[256];
  int err;
  char param_json[512];

  app_register_assets();

  snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "/user/app/%s/sce_sys", title_id);
  snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

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
  {
    int param_changed = write_owned_param(param_path, param_json, strlen(param_json));
    int icon_changed = write_owned_binary(icon_path, icon0_png, icon0_png_size);
    if(param_changed < 0 || icon_changed < 0) {
      perror("install launcher assets");
      return -1;
    }
    if(!param_changed && !icon_changed) {
      printf("Launcher app %s is already up to date on port %u\n",
             title_id, (unsigned int)port);
      return 0;
    }
  }
  /* AppInstallAll does not reliably refresh an already registered title on
   * every PS5 firmware. Do not remove our existing title until the replacement
   * directory has been completely written and synced; otherwise a transient
   * storage failure turns a refresh into a missing Home Screen launcher.
   * Only this payload's own title is ever removed. */
  (void)sceAppInstUtilAppUnInstall(title_id);
  if((err = install_app(title_id, "/user/app/"))) {
    printf("install_app: error 0x%08X\n", err);
    return -1;
  }

  return 0;
}
