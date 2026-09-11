#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef __SCE__
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "filemgr_internal.h"
#include "json_util.h"
#include "path_util.h"

#ifndef VERSION_TAG
#define VERSION_TAG "dev"
#endif

#define URL_DOWNLOAD_BUFFER_SIZE (64U * 1024U)
#define URL_DOWNLOAD_HTTP_POOL_SIZE (512U * 1024U)
#define URL_DOWNLOAD_TIMEOUT_US (20U * 1000U * 1000U)
#define URL_DOWNLOAD_MIN_FREE_BYTES (32ULL * 1024ULL * 1024ULL)
#define URL_DOWNLOAD_CHECKPOINT_BYTES (4ULL * 1024ULL * 1024ULL)
#define URL_DOWNLOAD_QUEUE_LIMIT 8u
#define URL_DOWNLOAD_JOURNAL_MAGIC 0x4d4b444cu
#define URL_DOWNLOAD_JOURNAL_VERSION 1u
#define URL_DOWNLOAD_JOURNAL_PREFIX "mkpfs-download-"
#define URL_DOWNLOAD_JOURNAL_SUFFIX ".resume"

typedef struct url_download_journal {
  uint32_t magic;
  uint32_t version;
  uint64_t checksum;
  uint64_t done;
  uint64_t total;
  char source[PATH_MAX];
  char destination[PATH_MAX];
  char temporary[PATH_MAX];
} url_download_journal_t;

typedef struct remote_url {
  int https;
#ifndef __SCE__
  char host[256];
  char port[8];
  char path[PATH_MAX];
#endif
} remote_url_t;

static uint64_t
download_hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;

  for(size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t
download_journal_checksum(const url_download_journal_t *journal) {
  return download_hash_bytes(UINT64_C(1469598103934665603), journal,
                             offsetof(url_download_journal_t, checksum));
}

static const char *
download_journal_directory(void) {
  const char *configured = getenv("WFM_DOWNLOAD_DIR");

  if(configured && configured[0] == '/') return configured;
#ifdef __linux__
  return "/tmp/mkpfs-downloads";
#else
  return "/data/mkpfs-downloads";
#endif
}

static int
ensure_download_journal_directory(void) {
  struct stat st;
  const char *directory = download_journal_directory();

  if(mkdir(directory, 0700) && errno != EEXIST) return -1;
  if(lstat(directory, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int
download_path_hash(const char *destination, uint64_t *hash) {
  if(!destination || !hash) {
    errno = EINVAL;
    return -1;
  }
  *hash = download_hash_bytes(UINT64_C(1469598103934665603), destination,
                              strlen(destination) + 1);
  return 0;
}

static int
download_journal_path(const char *destination, char *path, size_t path_size) {
  uint64_t hash;

  if(download_path_hash(destination, &hash) || ensure_download_journal_directory() ||
     snprintf(path, path_size, "%s/" URL_DOWNLOAD_JOURNAL_PREFIX "%016llx" \
              URL_DOWNLOAD_JOURNAL_SUFFIX, download_journal_directory(),
              (unsigned long long)hash) >= (int)path_size) {
    if(!errno) errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int
download_temporary_path(const char *destination, char *path, size_t path_size) {
  char parent[PATH_MAX];
  uint64_t hash;

  if(path_dirname(destination, parent, sizeof(parent)) ||
     download_path_hash(destination, &hash) ||
     snprintf(path, path_size, "%s/.mkpfs-download-%016llx.part", parent,
              (unsigned long long)hash) >= (int)path_size) {
    if(!errno) errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int
write_download_journal(const file_task_t *task) {
  url_download_journal_t journal = {0};
  char temporary[PATH_MAX];
  FILE *f;
  int fd;

  if(!task || !task->download_journal[0] || !task->download_temporary[0]) {
    errno = EINVAL;
    return -1;
  }
  if(snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX",
              task->download_journal) >= (int)sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  journal.magic = URL_DOWNLOAD_JOURNAL_MAGIC;
  journal.version = URL_DOWNLOAD_JOURNAL_VERSION;
  pthread_mutex_lock(&g_tasks_lock);
  journal.done = task->done;
  journal.total = task->total;
  snprintf(journal.source, sizeof(journal.source), "%s", task->src);
  snprintf(journal.destination, sizeof(journal.destination), "%s", task->dst);
  snprintf(journal.temporary, sizeof(journal.temporary), "%s", task->download_temporary);
  pthread_mutex_unlock(&g_tasks_lock);
  journal.checksum = download_journal_checksum(&journal);
  fd = mkstemp(temporary);
  if(fd < 0 || !(f = fdopen(fd, "wb"))) {
    if(fd >= 0) close(fd);
    return -1;
  }
  if(fwrite(&journal, sizeof(journal), 1, f) != 1 || fflush(f) ||
     fsync(fileno(f)) || fclose(f) || rename(temporary, task->download_journal)) {
    int error = errno ? errno : EIO;
    unlink(temporary);
    errno = error;
    return -1;
  }
  return 0;
}

void
url_download_discard_state(file_task_t *task) {
  if(!task) return;
  if(task->download_temporary[0]) unlink(task->download_temporary);
  if(task->download_journal[0]) unlink(task->download_journal);
}

#ifdef __SCE__
/* PS5 SceHttp interfaces.  The public payload SDK supplies the corresponding
 * import library but does not ship application headers for these symbols. */
extern int sceHttpInit(unsigned int pool_size);
extern int sceHttpTerm(void);
extern int sceHttpCreateTemplate(const char *user_agent, int http_version,
                                 int auto_proxy_config);
extern int sceHttpDeleteTemplate(int template_id);
extern int sceHttpCreateConnectionWithURL(int template_id, const char *url,
                                          int keep_alive);
extern int sceHttpDeleteConnection(int connection_id);
extern int sceHttpCreateRequestWithURL(int connection_id, int method,
                                       const char *url,
                                       unsigned long long content_length);
extern int sceHttpDeleteRequest(int request_id);
extern int sceHttpAddRequestHeader(int request_id, const char *name,
                                   const char *value, int mode);
extern int sceHttpSetAutoRedirect(int id, int enabled);
extern int sceHttpSetResolveTimeOut(int id, unsigned int microseconds);
extern int sceHttpSetResolveRetry(int id, int retry_count);
extern int sceHttpSetConnectTimeOut(int id, unsigned int microseconds);
extern int sceHttpSetSendTimeOut(int id, unsigned int microseconds);
extern int sceHttpSetRecvTimeOut(int id, unsigned int microseconds);
extern int sceHttpSetResponseHeaderMaxSize(int id, unsigned int bytes);
extern int sceHttpSendRequest(int request_id, const void *body,
                              unsigned int body_size);
extern int sceHttpGetStatusCode(int request_id, int *status_code);
extern int sceHttpGetResponseContentLength(int request_id,
                                           unsigned long long *content_length);
extern int sceHttpReadData(int request_id, void *buffer, unsigned int size);
extern int sceHttpsEnableOption(unsigned int ssl_flags);

#define SCE_HTTP_METHOD_GET 0
#define SCE_HTTP_VERSION_1_1 2
#define SCE_HTTP_ENABLE 1
#define SCE_HTTP_HEADER_OVERWRITE 0
#define SCE_HTTPS_FLAG_SERVER_VERIFY 0x01U
#define SCE_HTTPS_FLAG_CN_CHECK 0x04U
#define SCE_HTTPS_FLAG_NOT_AFTER_CHECK 0x08U
#define SCE_HTTPS_FLAG_NOT_BEFORE_CHECK 0x10U
#define SCE_HTTPS_FLAG_KNOWN_CA_CHECK 0x20U
#endif

static void
url_task_total(file_task_t *task, unsigned long long total) {
  pthread_mutex_lock(&g_tasks_lock);
  task->total = total;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

static int
url_has_control_characters(const char *text) {
  const unsigned char *p = (const unsigned char *)text;
  for(; *p; p++) {
    if(*p < 0x20 || *p == 0x7f) return 1;
  }
  return 0;
}

static int
parse_remote_url(const char *text, remote_url_t *out) {
  const char *authority;
  const char *end;
  size_t scheme_len;

  if(!text || !out || !text[0] || strlen(text) >= PATH_MAX ||
     url_has_control_characters(text)) {
    errno = EINVAL;
    return -1;
  }
  authority = strstr(text, "://");
  if(!authority) {
    errno = EINVAL;
    return -1;
  }
  scheme_len = (size_t)(authority - text);
  if(scheme_len == 4 && !strncasecmp(text, "http", 4)) {
    out->https = 0;
  } else if(scheme_len == 5 && !strncasecmp(text, "https", 5)) {
    out->https = 1;
  } else {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  authority += 3;
  end = authority;
  while(*end && *end != '/' && *end != '?' && *end != '#') end++;
  if(end == authority || memchr(authority, '@', (size_t)(end - authority))) {
    errno = EINVAL;
    return -1;
  }
#ifndef __SCE__
  {
    const char *host_start = authority;
    const char *host_end = end;
    const char *port = NULL;
    size_t host_len;

    if(*host_start == '[') {
      const char *close = memchr(host_start, ']', (size_t)(end - host_start));
      if(!close || close == host_start + 1 || close + 1 != end) {
        errno = EPROTONOSUPPORT;
        return -1;
      }
      errno = EPROTONOSUPPORT; /* Host regression intentionally uses IPv4/DNS. */
      return -1;
    }
    for(const char *p = host_start; p < end; p++) {
      if(*p == ':') {
        if(port) {
          errno = EINVAL;
          return -1;
        }
        port = p;
      }
    }
    if(port) {
      host_end = port;
      port++;
      if(port == end || (size_t)(end - port) >= sizeof(out->port)) {
        errno = EINVAL;
        return -1;
      }
      for(const char *p = port; p < end; p++) {
        if(!isdigit((unsigned char)*p)) {
          errno = EINVAL;
          return -1;
        }
      }
      memcpy(out->port, port, (size_t)(end - port));
      out->port[end - port] = 0;
    } else {
      snprintf(out->port, sizeof(out->port), "%s", out->https ? "443" : "80");
    }
    host_len = (size_t)(host_end - host_start);
    if(!host_len || host_len >= sizeof(out->host)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = 0;
    if(*end == '/') {
      if(strlen(end) >= sizeof(out->path)) {
        errno = ENAMETOOLONG;
        return -1;
      }
      snprintf(out->path, sizeof(out->path), "%s", end);
    } else if(*end == '?') {
      if(snprintf(out->path, sizeof(out->path), "/%s", end) >= (int)sizeof(out->path)) {
        errno = ENAMETOOLONG;
        return -1;
      }
    } else {
      snprintf(out->path, sizeof(out->path), "/");
    }
  }
#endif
  return 0;
}

#ifndef __SCE__
typedef struct host_http_client {
  int fd;
  int status_code;
  unsigned long long content_length;
  unsigned long long body_remaining;
  char pending[8192];
  size_t pending_offset;
  size_t pending_size;
} host_http_client_t;

static int
host_connect(const remote_url_t *url) {
  struct addrinfo hints;
  struct addrinfo *results = NULL;
  struct addrinfo *entry;
  int fd = -1;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  if((ret = getaddrinfo(url->host, url->port, &hints, &results)) != 0) {
    errno = EHOSTUNREACH;
    return -1;
  }
  for(entry = results; entry; entry = entry->ai_next) {
    struct timeval timeout = { .tv_sec = 20, .tv_usec = 0 };
    fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if(fd < 0) continue;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if(!connect(fd, entry->ai_addr, entry->ai_addrlen)) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(results);
  return fd;
}

static int
host_send_all(int fd, const char *data, size_t size) {
  size_t sent = 0;
  while(sent < size) {
    ssize_t result = send(fd, data + sent, size - sent, 0);
    if(result <= 0) return -1;
    sent += (size_t)result;
  }
  return 0;
}

static void host_http_close(host_http_client_t *client);

static int
host_http_open(const char *text, const remote_url_t *url,
               unsigned long long resume_at, host_http_client_t *client,
               unsigned long long *content_length,
               char *error, size_t error_size) {
  char request[PATH_MAX + 384];
  char range_header[64] = {0};
  size_t used = 0;
  int status = 0;
  int saw_length = 0;

  (void)text;
  if(url->https) {
    snprintf(error, error_size, "HTTPS downloads require a PS5 runtime");
    errno = ENOTSUP;
    return -1;
  }
  memset(client, 0, sizeof(*client));
  if(resume_at && snprintf(range_header, sizeof(range_header),
                           "Range: bytes=%llu-\r\n", resume_at) >=
                  (int)sizeof(range_header)) {
    errno = EOVERFLOW;
    return -1;
  }
  client->fd = host_connect(url);
  if(client->fd < 0) {
    snprintf(error, error_size, "could not connect to download host");
    return -1;
  }
  if(snprintf(request, sizeof(request),
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MkPFS-PS5/" VERSION_TAG "\r\n"
              "Accept: */*\r\n%sConnection: close\r\n\r\n", url->path, url->host,
              range_header) >=
     (int)sizeof(request) || host_send_all(client->fd, request, strlen(request))) {
    snprintf(error, error_size, "could not send download request");
    host_http_close(client);
    return -1;
  }
  while(used + 1 < sizeof(client->pending)) {
    ssize_t got = recv(client->fd, client->pending + used,
                       sizeof(client->pending) - used - 1, 0);
    char *headers_end;
    if(got <= 0) {
      snprintf(error, error_size, "invalid HTTP response");
      errno = EPROTO;
      host_http_close(client);
      return -1;
    }
    used += (size_t)got;
    client->pending[used] = 0;
    headers_end = strstr(client->pending, "\r\n\r\n");
    if(headers_end) {
      char *line_end;
      char *line;
      size_t header_bytes = (size_t)(headers_end + 4 - client->pending);
      if(sscanf(client->pending, "HTTP/%*u.%*u %d", &status) != 1 ||
         status < 200 || status >= 300 ||
         (resume_at && status != 206 && status != 200) ||
         (!resume_at && status != 200)) {
        snprintf(error, error_size, "HTTP status %d", status);
        errno = EIO;
        host_http_close(client);
        return -1;
      }
      client->status_code = status;
      line = strstr(client->pending, "\r\n");
      while(line && line < headers_end) {
        char *value;
        line += 2;
        line_end = strstr(line, "\r\n");
        if(!line_end || line_end > headers_end) break;
        value = strchr(line, ':');
        if(value && (size_t)(value - line) == 14 &&
           !strncasecmp(line, "Content-Length", 14)) {
          char *endp;
          unsigned long long length;
          value++;
          while(value < line_end && isspace((unsigned char)*value)) value++;
          errno = 0;
          length = strtoull(value, &endp, 10);
          if(errno || endp == value || endp != line_end) {
            snprintf(error, error_size, "invalid Content-Length");
            errno = EPROTO;
            host_http_close(client);
            return -1;
          }
          client->content_length = length;
          saw_length = 1;
        }
        line = line_end;
      }
      if(strstr(client->pending, "Transfer-Encoding:") ||
         strstr(client->pending, "transfer-encoding:")) {
        snprintf(error, error_size, "chunked HTTP responses are not supported by host tests");
        errno = ENOTSUP;
        host_http_close(client);
        return -1;
      }
      client->pending_offset = header_bytes;
      client->pending_size = used;
      client->body_remaining = saw_length ? client->content_length : ULLONG_MAX;
      *content_length = saw_length ? client->content_length +
        (status == 206 ? resume_at : 0) : 0;
      return 0;
    }
  }
  snprintf(error, error_size, "HTTP response headers are too large");
  errno = EOVERFLOW;
  host_http_close(client);
  return -1;
}

static ssize_t
host_http_read(host_http_client_t *client, void *buffer, size_t size) {
  if(client->pending_offset < client->pending_size) {
    size_t available = client->pending_size - client->pending_offset;
    size_t take = available < size ? available : size;
    if(client->body_remaining != ULLONG_MAX && take > client->body_remaining) {
      take = (size_t)client->body_remaining;
    }
    memcpy(buffer, client->pending + client->pending_offset, take);
    client->pending_offset += take;
    if(client->body_remaining != ULLONG_MAX) client->body_remaining -= take;
    return (ssize_t)take;
  }
  if(client->body_remaining == 0) return 0;
  {
    ssize_t result = recv(client->fd, buffer, size, 0);
    if(result > 0 && client->body_remaining != ULLONG_MAX) {
      if((unsigned long long)result > client->body_remaining) {
        result = (ssize_t)client->body_remaining;
      }
      client->body_remaining -= (unsigned long long)result;
    }
    return result;
  }
}

static void
host_http_close(host_http_client_t *client) {
  if(client->fd >= 0) close(client->fd);
  client->fd = -1;
}
#endif

static int
sync_download_output(int fd, const char *parent) {
  int parent_fd;
  if(fsync(fd) && errno != EINVAL && errno != ENOTSUP) return -1;
  parent_fd = open(parent, O_RDONLY | O_DIRECTORY);
  if(parent_fd < 0) return -1;
  if(fsync(parent_fd) && errno != EINVAL && errno != ENOTSUP) {
    int saved = errno;
    close(parent_fd);
    errno = saved;
    return -1;
  }
  close(parent_fd);
  return 0;
}

static int
publish_download_file(const char *temporary, const char *destination,
                      const char *parent) {
  struct stat st;
  if(link(temporary, destination) == 0) {
    if(unlink(temporary)) return -1;
  } else if(errno == EOPNOTSUPP || errno == EPERM || errno == EXDEV) {
    /* FAT/exFAT does not provide hard links.  It still receives a same-directory
     * atomic rename after a second no-overwrite existence check. */
    if(lstat(destination, &st) == 0 || errno != ENOENT) {
      if(errno == ENOENT) errno = EEXIST;
      return -1;
    }
    if(rename(temporary, destination)) return -1;
  } else {
    return -1;
  }
  {
    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY);
    if(parent_fd >= 0) {
      if(fsync(parent_fd) && errno != EINVAL && errno != ENOTSUP) {
        int saved = errno;
        close(parent_fd);
        errno = saved;
        return -1;
      }
      close(parent_fd);
    }
  }
  return 0;
}

int
url_download_task_run(file_task_t *task) {
  char parent[PATH_MAX];
  char temporary[PATH_MAX];
  char error[160] = {0};
  remote_url_t url = {0};
  unsigned char buffer[URL_DOWNLOAD_BUFFER_SIZE];
  unsigned long long content_length = 0;
  unsigned long long resume_at = 0;
  struct stat temporary_st;
  int fd = -1;
  int created = 0;
  int result = EIO;

  if(!task || parse_remote_url(task->src, &url) ||
     path_dirname(task->dst, parent, sizeof(parent)) ||
     (!task->download_temporary[0] &&
      download_temporary_path(task->dst, task->download_temporary,
                              sizeof(task->download_temporary))) ||
     (!task->download_journal[0] &&
      download_journal_path(task->dst, task->download_journal,
                            sizeof(task->download_journal))) ||
     snprintf(temporary, sizeof(temporary), "%s", task->download_temporary) >=
       (int)sizeof(temporary)) {
    return errno ? errno : EINVAL;
  }
  if(lstat(temporary, &temporary_st) == 0) {
    if(!S_ISREG(temporary_st.st_mode) || temporary_st.st_nlink != 1 ||
       temporary_st.st_size < 0) {
      errno = EINVAL;
      return EINVAL;
    }
    resume_at = (unsigned long long)temporary_st.st_size;
    fd = open(temporary, O_WRONLY | O_APPEND | O_NOFOLLOW);
  } else if(errno == ENOENT) {
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    created = 1;
  }
  if(fd < 0) return errno;
  if(fchmod_0777(fd)) {
    result = errno;
    goto done;
  }
  if(resume_at) {
    pthread_mutex_lock(&g_tasks_lock);
    if(task->done != resume_at) task->done = resume_at;
    pthread_mutex_unlock(&g_tasks_lock);
  }
  if(write_download_journal(task)) {
    result = errno ? errno : EIO;
    goto done;
  }
  task_update(task, TASK_RUNNING, task->src, 0, NULL);

#ifdef __SCE__
  {
    int template_id = -1;
    int connection_id = -1;
    int request_id = -1;
    int status_code = 0;
    int http_initialized = 0;
    int code;
    char range_value[48];

    code = sceHttpInit(URL_DOWNLOAD_HTTP_POOL_SIZE);
    if(code < 0) {
      snprintf(error, sizeof(error), "PS5 HTTP initialization failed (%d)", code);
      goto sce_done;
    }
    http_initialized = 1;
    if(url.https && sceHttpsEnableOption(SCE_HTTPS_FLAG_SERVER_VERIFY |
                                         SCE_HTTPS_FLAG_CN_CHECK |
                                         SCE_HTTPS_FLAG_NOT_AFTER_CHECK |
                                         SCE_HTTPS_FLAG_NOT_BEFORE_CHECK |
                                         SCE_HTTPS_FLAG_KNOWN_CA_CHECK) < 0) {
      snprintf(error, sizeof(error), "PS5 HTTPS certificate verification setup failed");
      goto sce_done;
    }
    template_id = sceHttpCreateTemplate("MkPFS-PS5/" VERSION_TAG,
                                        SCE_HTTP_VERSION_1_1, 1);
    if(template_id < 0 || sceHttpSetAutoRedirect(template_id, 0) < 0 ||
       sceHttpSetResolveTimeOut(template_id, URL_DOWNLOAD_TIMEOUT_US) < 0 ||
       sceHttpSetResolveRetry(template_id, 1) < 0 ||
       sceHttpSetConnectTimeOut(template_id, URL_DOWNLOAD_TIMEOUT_US) < 0 ||
       sceHttpSetSendTimeOut(template_id, URL_DOWNLOAD_TIMEOUT_US) < 0 ||
       sceHttpSetRecvTimeOut(template_id, URL_DOWNLOAD_TIMEOUT_US) < 0 ||
       sceHttpSetResponseHeaderMaxSize(template_id, 32U * 1024U) < 0) {
      snprintf(error, sizeof(error), "PS5 HTTP session setup failed");
      goto sce_done;
    }
    connection_id = sceHttpCreateConnectionWithURL(template_id, task->src, 0);
    request_id = connection_id < 0 ? -1 :
      sceHttpCreateRequestWithURL(connection_id, SCE_HTTP_METHOD_GET, task->src, 0);
    if(resume_at && snprintf(range_value, sizeof(range_value), "bytes=%llu-",
                             resume_at) >= (int)sizeof(range_value)) {
      snprintf(error, sizeof(error), "download range is invalid");
      goto sce_done;
    }
    if(connection_id < 0 || request_id < 0 ||
       (resume_at && sceHttpAddRequestHeader(request_id, "Range", range_value,
                                              SCE_HTTP_HEADER_OVERWRITE) < 0) ||
       sceHttpSendRequest(request_id, NULL, 0) < 0 ||
       sceHttpGetStatusCode(request_id, &status_code) < 0) {
      snprintf(error, sizeof(error), "PS5 HTTP request failed");
      goto sce_done;
    }
    if(status_code < 200 || status_code >= 300 ||
       (resume_at && status_code != 206 && status_code != 200) ||
       (!resume_at && status_code != 200)) {
      snprintf(error, sizeof(error), "HTTP status %d", status_code);
      goto sce_done;
    }
    if(status_code == 200 && resume_at) {
      if(ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0) {
        result = errno;
        goto sce_done;
      }
      resume_at = 0;
      pthread_mutex_lock(&g_tasks_lock);
      task->done = 0;
      task->download_checkpoint_done = 0;
      pthread_mutex_unlock(&g_tasks_lock);
      if(write_download_journal(task)) {
        result = errno ? errno : EIO;
        goto sce_done;
      }
    }
    if(sceHttpGetResponseContentLength(request_id, &content_length) == 0 &&
       content_length != ULLONG_MAX) {
      url_task_total(task, content_length + resume_at);
    } else {
      content_length = 0;
    }
    while(!task_cancel_requested(task) &&
          !atomic_load_explicit(&task->pause_requested, memory_order_acquire)) {
      int got = sceHttpReadData(request_id, buffer, sizeof(buffer));
      if(got < 0) {
        if(task_cancel_requested(task)) {
          result = ECANCELED;
          goto sce_done;
        }
        snprintf(error, sizeof(error), "PS5 HTTP response read failed");
        goto sce_done;
      }
      if(got == 0) {
        result = 0;
        break;
      }
      {
        size_t offset = 0;
        while(offset < (size_t)got) {
          ssize_t wrote = write(fd, buffer + offset, (size_t)got - offset);
          if(wrote < 0) {
            result = errno;
            goto sce_done;
          }
          offset += (size_t)wrote;
        }
      }
      task_update(task, TASK_RUNNING, task->src, (unsigned long long)got, NULL);
      if(task->done - task->download_checkpoint_done >= URL_DOWNLOAD_CHECKPOINT_BYTES) {
        if(write_download_journal(task)) {
          result = errno ? errno : EIO;
          goto sce_done;
        }
        task->download_checkpoint_done = task->done;
      }
    }
    if(task_cancel_requested(task)) result = ECANCELED;
    else if(atomic_load_explicit(&task->pause_requested, memory_order_acquire)) result = EINPROGRESS;

sce_done:
    if(request_id >= 0) sceHttpDeleteRequest(request_id);
    if(connection_id >= 0) sceHttpDeleteConnection(connection_id);
    if(template_id >= 0) sceHttpDeleteTemplate(template_id);
    if(http_initialized) sceHttpTerm();
  }
#else
  {
    host_http_client_t client;
    ssize_t got = 0;
    memset(&client, 0, sizeof(client));
    client.fd = -1;
    if(host_http_open(task->src, &url, resume_at, &client, &content_length,
                      error, sizeof(error))) {
      result = errno ? errno : EIO;
      goto done;
    }
    if(client.status_code == 200 && resume_at) {
      if(ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0) {
        result = errno;
        goto done;
      }
      resume_at = 0;
      pthread_mutex_lock(&g_tasks_lock);
      task->done = 0;
      task->download_checkpoint_done = 0;
      pthread_mutex_unlock(&g_tasks_lock);
      if(write_download_journal(task)) {
        result = errno ? errno : EIO;
        goto done;
      }
    }
    if(content_length) url_task_total(task, content_length);
    while(!task_cancel_requested(task) &&
          !atomic_load_explicit(&task->pause_requested, memory_order_acquire) &&
          (got = host_http_read(&client, buffer, sizeof(buffer))) > 0) {
      size_t offset = 0;
      while(offset < (size_t)got) {
        ssize_t wrote = write(fd, buffer + offset, (size_t)got - offset);
        if(wrote < 0) {
          result = errno;
          host_http_close(&client);
          goto done;
        }
        offset += (size_t)wrote;
      }
      task_update(task, TASK_RUNNING, task->src, (unsigned long long)got, NULL);
      if(task->done - task->download_checkpoint_done >= URL_DOWNLOAD_CHECKPOINT_BYTES) {
        if(write_download_journal(task)) {
          result = errno ? errno : EIO;
          host_http_close(&client);
          goto done;
        }
        task->download_checkpoint_done = task->done;
      }
    }
    if(task_cancel_requested(task)) {
      result = ECANCELED;
    } else if(atomic_load_explicit(&task->pause_requested, memory_order_acquire)) {
      result = EINPROGRESS;
    } else if(got < 0) {
      snprintf(error, sizeof(error), "HTTP response read failed");
      result = errno ? errno : EIO;
    } else {
      result = 0;
    }
    host_http_close(&client);
  }
#endif

  if(result == 0 && content_length) {
    unsigned long long done;
    pthread_mutex_lock(&g_tasks_lock);
    done = task->done;
    pthread_mutex_unlock(&g_tasks_lock);
    if(done != content_length) {
      snprintf(error, sizeof(error), "download ended before Content-Length");
      result = EIO;
    }
  }
  if(result == 0 && sync_download_output(fd, parent)) {
    result = errno ? errno : EIO;
  }
  if(result == 0 && task_cancel_requested(task)) result = ECANCELED;
  if(result == 0 && publish_download_file(temporary, task->dst, parent)) {
    result = errno ? errno : EIO;
  }

done:
  if(fd >= 0) close(fd);
  if(result == 0) {
    url_download_discard_state(task);
  } else if(result == ECANCELED) {
    url_download_discard_state(task);
  } else if(result == EINPROGRESS) {
    if(write_download_journal(task)) result = errno ? errno : EIO;
  } else if(created || task->download_temporary[0]) {
    /* Keep a validated same-directory part file plus its atomic journal. A
     * retry or a payload restart asks the remote endpoint for the exact range
     * already durable on disk; it never publishes a partial final name. */
    (void)write_download_journal(task);
  }
  if(result != 0 && error[0]) {
    task_update(task, TASK_RUNNING, task->src, 0, error);
  }
  return result;
}

static int
read_download_journal(const char *path, url_download_journal_t *journal) {
  FILE *f;
  struct stat st;

  if(!path || !journal || lstat(path, &st) || !S_ISREG(st.st_mode) ||
     st.st_nlink != 1 || (size_t)st.st_size != sizeof(*journal) ||
     !(f = fopen(path, "rb"))) return -1;
  if(fread(journal, sizeof(*journal), 1, f) != 1 || fclose(f) ||
     journal->magic != URL_DOWNLOAD_JOURNAL_MAGIC ||
     journal->version != URL_DOWNLOAD_JOURNAL_VERSION ||
     journal->checksum != download_journal_checksum(journal) ||
     !memchr(journal->source, 0, sizeof(journal->source)) ||
     !memchr(journal->destination, 0, sizeof(journal->destination)) ||
     !memchr(journal->temporary, 0, sizeof(journal->temporary))) {
    return -1;
  }
  return 0;
}

static int
restore_download_part(const url_download_journal_t *journal) {
  struct stat st;
  int fd;

  if(lstat(journal->temporary, &st) || !S_ISREG(st.st_mode) ||
     st.st_nlink != 1 || st.st_size < 0 || (uint64_t)st.st_size < journal->done) {
    errno = EINVAL;
    return -1;
  }
  /* A crash can occur after writing a block but before the next atomic
   * checkpoint. Discard only that unjournaled suffix, so the next Range
   * request begins at an explicitly durable offset rather than trusting it. */
  if((uint64_t)st.st_size > journal->done) {
    fd = open(journal->temporary, O_WRONLY | O_NOFOLLOW);
    if(fd < 0) return -1;
    if(ftruncate(fd, (off_t)journal->done) || fsync(fd)) {
      int error = errno;
      close(fd);
      errno = error;
      return -1;
    }
    close(fd);
  }
  return 0;
}

int
url_download_resume_interrupted(void) {
  DIR *directory;
  struct dirent *entry;
  unsigned int restored = 0;

  if(ensure_download_journal_directory()) return -1;
  if(has_active_task()) return 0;
  directory = opendir(download_journal_directory());
  if(!directory) return -1;
  while(restored < URL_DOWNLOAD_QUEUE_LIMIT && (entry = readdir(directory)) != NULL) {
    char path[PATH_MAX];
    char expected_journal[PATH_MAX];
    char expected_temporary[PATH_MAX];
    url_download_journal_t journal;
    remote_url_t parsed = {0};
    struct stat destination_st;
    file_task_t *task;
    pthread_t thread;

    if(strncmp(entry->d_name, URL_DOWNLOAD_JOURNAL_PREFIX,
               strlen(URL_DOWNLOAD_JOURNAL_PREFIX)) ||
       strlen(entry->d_name) <= strlen(URL_DOWNLOAD_JOURNAL_PREFIX) +
                              strlen(URL_DOWNLOAD_JOURNAL_SUFFIX) ||
       strcmp(entry->d_name + strlen(entry->d_name) -
              strlen(URL_DOWNLOAD_JOURNAL_SUFFIX), URL_DOWNLOAD_JOURNAL_SUFFIX) ||
       snprintf(path, sizeof(path), "%s/%s", download_journal_directory(),
                entry->d_name) >= (int)sizeof(path) ||
       read_download_journal(path, &journal) || parse_remote_url(journal.source, &parsed) ||
       path_dirname(journal.destination, expected_journal, sizeof(expected_journal)) ||
       lstat(expected_journal, &destination_st) || !S_ISDIR(destination_st.st_mode) ||
       lstat(journal.destination, &destination_st) == 0 || errno != ENOENT ||
       download_journal_path(journal.destination, expected_journal,
                             sizeof(expected_journal)) ||
       download_temporary_path(journal.destination, expected_temporary,
                               sizeof(expected_temporary)) ||
       strcmp(path, expected_journal) || strcmp(journal.temporary, expected_temporary) ||
       restore_download_part(&journal)) {
      continue;
    }
    task = calloc(1, sizeof(*task));
    if(!task) break;
    atomic_init(&task->cancel_requested, 0);
    atomic_init(&task->pause_requested, 0);
    task->op = TASK_URL_DOWNLOAD;
    task->state = TASK_QUEUED;
    task->done = journal.done;
    task->total = journal.total;
    task->download_checkpoint_done = journal.done;
    snprintf(task->src, sizeof(task->src), "%s", journal.source);
    snprintf(task->dst, sizeof(task->dst), "%s", journal.destination);
    snprintf(task->current, sizeof(task->current), "%s", journal.source);
    snprintf(task->download_journal, sizeof(task->download_journal), "%s", path);
    snprintf(task->download_temporary, sizeof(task->download_temporary), "%s", journal.temporary);
    task->created_at = task->updated_at = time(NULL);
    pthread_mutex_lock(&g_tasks_lock);
    task->id = g_next_task_id++;
    task->next = g_tasks;
    g_tasks = task;
    pthread_mutex_unlock(&g_tasks_lock);
    if(pthread_create(&thread, NULL, task_worker, task)) {
      task_update(task, TASK_FAILED, task->src, 0, "download recovery worker creation failed");
    } else {
      pthread_detach(thread);
      restored++;
    }
  }
  closedir(directory);
  return (int)restored;
}

enum MHD_Result
api_url_download(struct MHD_Connection *conn) {
  char *url = query_value(conn, "url");
  char *destination = absolute_path_value(query_value(conn, "destination"));
  char *name = fs_path_value(query_value(conn, "name"));
  char output[PATH_MAX];
  char journal_path[PATH_MAX];
  struct stat destination_st;
  struct stat output_st;
  file_task_t *task = NULL;
  strbuf_t b = {0};
  unsigned long long available;
  remote_url_t parsed = {0};
  int status = MHD_HTTP_BAD_REQUEST;

  if(!url || !destination || !name || strchr(name, '/') ||
     !relative_path_safe(name) || parse_remote_url(url, &parsed) ||
     lstat(destination, &destination_st) || !S_ISDIR(destination_st.st_mode) ||
     path_join(output, sizeof(output), destination, name)) {
    goto out;
  }
  if(lstat(output, &output_st) == 0 || errno != ENOENT) {
    status = MHD_HTTP_CONFLICT;
    goto out;
  }
  if(download_journal_path(output, journal_path, sizeof(journal_path)) ||
     lstat(journal_path, &output_st) == 0 || errno != ENOENT) {
    status = MHD_HTTP_CONFLICT;
    goto out;
  }
  if(target_available_space(destination, &available) ||
     available < URL_DOWNLOAD_MIN_FREE_BYTES) {
    status = MHD_HTTP_INSUFFICIENT_STORAGE;
    goto out;
  }
  task = calloc(1, sizeof(*task));
  if(!task) {
    status = MHD_HTTP_INTERNAL_SERVER_ERROR;
    goto out;
  }
  atomic_init(&task->cancel_requested, 0);
  atomic_init(&task->pause_requested, 0);
  task->op = TASK_URL_DOWNLOAD;
  task->state = TASK_QUEUED;
  snprintf(task->src, sizeof(task->src), "%s", url);
  snprintf(task->dst, sizeof(task->dst), "%s", output);
  snprintf(task->current, sizeof(task->current), "%s", url);
  task->created_at = task->updated_at = time(NULL);

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  {
    file_task_t *existing;
    unsigned int url_tasks = 0;
    int blocking_task = 0;
    for(existing = g_tasks; existing; existing = existing->next) {
      if(existing->op == TASK_URL_DOWNLOAD &&
         (task_is_active(existing) || existing->state == TASK_PAUSED)) {
        url_tasks++;
      } else if(existing->op != TASK_URL_DOWNLOAD && task_is_active(existing)) {
        blocking_task = 1;
      }
    }
    if(blocking_task || url_tasks >= URL_DOWNLOAD_QUEUE_LIMIT) {
    pthread_mutex_unlock(&g_tasks_lock);
    status = MHD_HTTP_CONFLICT;
    goto out;
    }
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);
  if(pthread_create(&task->thread, NULL, task_worker, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  free(url);
  free(destination);
  free(name);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");

out:
  free(url);
  free(destination);
  free(name);
  free(task);
  return send_json_error(conn, status,
                         status == MHD_HTTP_CONFLICT ? "destination exists or another task is running" :
                         status == MHD_HTTP_INSUFFICIENT_STORAGE ? "insufficient storage" :
                         "invalid URL download request");
}
