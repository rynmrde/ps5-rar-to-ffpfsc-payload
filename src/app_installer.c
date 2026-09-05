#include "app_installer.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
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

static int
install_file(const char *path, const uint8_t *data, size_t size) {
  struct stat st;
  FILE *f;

  if(!stat(path, &st)) {
    return 0;
  }
  if(errno != ENOENT) {
    return -1;
  }
  if(!(f = fopen(path, "wb"))) {
    return -1;
  }
  if(fwrite(data, size, 1, f) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
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
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
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
  if(fclose(f)) {
    unlink(tmp);
    return -1;
  }
  if(rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  return 0;
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

  asset_register("/icon0.png", icon0_png, icon0_png_size, "image/png", 0);

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
           "    \"en-US\": {\"titleName\": \"Web File Manager\"},\n"
           "    \"zh-Hans\": {\"titleName\": \"文件管理器网页\"},\n"
           "    \"zh-Hant\": {\"titleName\": \"文件管理器网页\"}\n"
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
  if(write_owned_param(param_path, param_json, strlen(param_json)) ||
     install_file(icon_path, icon0_png, icon0_png_size)) {
    perror("install launcher assets");
    return -1;
  }
  if((err = install_app(title_id, "/user/app/"))) {
    printf("install_app: error 0x%08X\n", err);
    return -1;
  }

  return 0;
}
