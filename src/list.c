#include "filemgr_internal.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json_util.h"
#include "path_util.h"

static char
mode_type(const struct stat *st) {
  if(S_ISDIR(st->st_mode)) return 'd';
  if(S_ISLNK(st->st_mode)) return 'l';
  if(S_ISCHR(st->st_mode)) return 'c';
  if(S_ISBLK(st->st_mode)) return 'b';
  if(S_ISFIFO(st->st_mode)) return 'p';
  if(S_ISSOCK(st->st_mode)) return 's';
  return '-';
}

enum MHD_Result
api_list(struct MHD_Connection *conn) {
  char *path = absolute_path_value(query_value(conn, "path"));
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  strbuf_t b = {0};
  int first = 1;

  if(has_active_task()) {
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }

  if(!path) {
    path = strdup("/");
    if(!path) {
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "out of memory");
    }
  }
  if(!(dir = opendir(path))) {
    fprintf(stderr, "api_list: opendir(%s) failed errno=%d\n", path, errno);
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, NULL);
  }

  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_escape(&b, path);
  strbuf_append(&b, ",\"parent\":");
  char parent[PATH_MAX];
  if(path_dirname(path, parent, sizeof(parent))) {
    strcpy(parent, "/");
  }
  json_escape(&b, parent);
  strbuf_append(&b, ",\"entries\":[");

  while((entry = readdir(dir))) {
    char child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       lstat(child, &st)) {
      continue;
    }

    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_append(&b, "{\"name\":");
    json_escape(&b, entry->d_name);
    strbuf_append(&b, ",\"path\":");
    json_escape(&b, child);
    strbuf_printf(&b, ",\"type\":\"%c\",\"mode\":%u,\"size\":%lld,\"mtime\":%lld}",
                  mode_type(&st), (unsigned int)(st.st_mode & 07777),
                  (long long)st.st_size, (long long)st.st_mtime);
  }

  closedir(dir);
  fprintf(stderr, "api_list: path=%s entries=%s\n", path, first ? "0" : ">0");
  free(path);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_roots(struct MHD_Connection *conn) {
  static const char *const candidates[] = {
    "/", "/user/app", "/data", "/mnt", "/mnt/usb0", "/mnt/usb1",
    "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6",
    "/mnt/usb7", "/mnt/ext0", "/mnt/ext1"
  };
  strbuf_t b = {0};
  int first = 1;

  strbuf_append(&b, "{\"ok\":true,\"roots\":[");
  for(size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    DIR *dir = opendir(candidates[i]);
    if(!dir) {
      fprintf(stderr, "api_roots: unreadable path=%s errno=%d\n",
              candidates[i], errno);
      continue;
    }
    closedir(dir);
    fprintf(stderr, "api_roots: readable path=%s\n", candidates[i]);
    if(!first) strbuf_append(&b, ",");
    first = 0;
    json_escape(&b, candidates[i]);
  }
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}
