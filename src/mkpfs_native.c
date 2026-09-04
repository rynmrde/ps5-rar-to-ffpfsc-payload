#include "mkpfs_native.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int append_component(char *out, size_t cap, size_t *used,
                            const char *component) {
  size_t n = strlen(component);
  if (!n || !strcmp(component, ".") || !strcmp(component, "..")) return EINVAL;
  if (strchr(component, '\\')) return EINVAL;
  if (*used + n + 1 >= cap) return ENAMETOOLONG;
  if (*used > 1) out[(*used)++] = '/';
  memcpy(out + *used, component, n);
  *used += n;
  out[*used] = '\0';
  return 0;
}

int mkpfs_normalize_path(const char *input, char *output, size_t output_size) {
  const char *p;
  size_t used = 0;
  if (!input || !output || output_size < 2 || input[0] != '/') return EINVAL;
  output[used++] = '/'; output[used] = '\0';
  p = input + 1;
  while (*p) {
    const char *start = p;
    size_t n;
    char component[NAME_MAX + 1];
    while (*p && *p != '/') p++;
    n = (size_t)(p - start);
    if (n > NAME_MAX) return ENAMETOOLONG;
    if (n) {
      memcpy(component, start, n); component[n] = '\0';
      if (append_component(output, output_size, &used, component)) return EINVAL;
    }
    while (*p == '/') p++;
  }
  return 0;
}

static int scan_dir(const char *path, mkpfs_scan_result_t *result) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  if (!dir) return errno;
  while ((entry = readdir(dir)) != NULL) {
    char child[PATH_MAX];
    struct stat st;
    int rc;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) { closedir(dir); return ENAMETOOLONG; }
    if (lstat(child, &st) != 0) { rc = errno; closedir(dir); return rc; }
    if (S_ISREG(st.st_mode)) { result->file_count++; result->total_bytes += (uint64_t)st.st_size; }
    else if (S_ISDIR(st.st_mode)) { result->directory_count++; rc = scan_dir(child, result); if (rc) { closedir(dir); return rc; } }
    else if (S_ISLNK(st.st_mode)) { closedir(dir); return ELOOP; }
  }
  closedir(dir);
  return 0;
}

int mkpfs_scan_folder(const char *root, mkpfs_scan_result_t *result) {
  struct stat st;
  if (!root || !result || lstat(root, &st) != 0) return errno;
  if (!S_ISDIR(st.st_mode)) return ENOTDIR;
  memset(result, 0, sizeof(*result));
  return scan_dir(root, result);
}

int mkpfs_convert_folder(const char *source, const char *destination,
                         const char *output_name,
                         const mkpfs_native_options_t *options,
                         volatile int *cancel_requested) {
  mkpfs_scan_result_t scan;
  char normalized[PATH_MAX];
  (void)destination; (void)output_name; (void)options; (void)cancel_requested;
  if (mkpfs_normalize_path(source, normalized, sizeof(normalized)) != 0) return EINVAL;
  if (mkpfs_scan_folder(normalized, &scan) != 0) return errno ? errno : EIO;
  /* Never emit a suffix-only artifact. A real writer must be linked here. */
  return ENOTSUP;
}
