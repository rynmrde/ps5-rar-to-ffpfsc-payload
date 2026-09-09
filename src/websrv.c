#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "asset.h"
#include "filemgr.h"
#include "websrv.h"

#define REQUEST_BODY_MAX (4 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_LIMIT (512 * 1024)
#define HTTP_CONNECTION_MEMORY_INCREMENT (64 * 1024)
#define HTTP_SOCKET_RCVBUF_SIZE (512 * 1024)
#define HTTP_SOCKET_SNDBUF_SIZE (512 * 1024)
#define HTTP_CONNECTION_LIMIT 32u
#define HTTP_PER_IP_CONNECTION_LIMIT 16u
#define HTTP_CONNECTION_TIMEOUT_SECONDS 120u
#define HTTP_ACCESS_TOKEN_MAX 64u

static int
websrv_accept_error_retryable(int error) {
  switch(error) {
  case EAGAIN:
#if EWOULDBLOCK != EAGAIN
  case EWOULDBLOCK:
#endif
  case ECONNABORTED:
  case ENOBUFS:
  case ENOMEM:
  case ENETDOWN:
  case ENETUNREACH:
  case EHOSTDOWN:
  case EHOSTUNREACH:
  case EPROTO:
  case EIO:
    return 1;
  default:
    return 0;
  }
}

static volatile sig_atomic_t g_stop_requested;
static int g_listen_fd = -1;
static char g_access_token[HTTP_ACCESS_TOKEN_MAX + 1];
static websrv_ready_callback_t g_ready_callback;
static void *g_ready_callback_arg;

void
websrv_set_ready_callback(websrv_ready_callback_t callback, void *arg) {
  g_ready_callback = callback;
  g_ready_callback_arg = arg;
}

static void
websrv_tune_connection_socket(int fd) {
  const int sndbuf = HTTP_SOCKET_SNDBUF_SIZE;
  const int nodelay = 1;

  /* Keep a practical window without reserving 4 MiB per accepted peer. */
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

typedef struct request_context {
  char *body;
  size_t size;
  int too_large;
  int authorized;
  int upload_stream;
  void *upload_ctx;
} request_context_t;

int
websrv_set_access_token(const char *token) {
  size_t length;

  if(!token || !(length = strlen(token)) || length > HTTP_ACCESS_TOKEN_MAX) {
    return -1;
  }
  memcpy(g_access_token, token, length);
  g_access_token[length] = 0;
  return 0;
}

static int
websrv_token_matches(const char *candidate) {
  size_t expected_length = strlen(g_access_token);
  size_t candidate_length;
  unsigned char difference = 0;

  if(!candidate || !expected_length ||
     (candidate_length = strlen(candidate)) != expected_length) {
    return 0;
  }
  for(size_t i = 0; i < candidate_length; i++) {
    difference |= (unsigned char)(candidate[i] ^ g_access_token[i]);
  }
  return difference == 0;
}

static int
websrv_authorized(struct MHD_Connection *conn) {
  const char *token = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                   "X-WFM-Token");
  if(!token) {
    token = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "token");
  }
  return websrv_token_matches(token);
}

static enum MHD_Result
websrv_access_denied(struct MHD_Connection *conn) {
  static const char json[] =
    "{\"ok\":false,\"error\":\"access denied\",\"error_code\":\"access_denied\",\"error_arg\":\"\"}";
  struct MHD_Response *resp = MHD_create_response_from_buffer(
    sizeof(json) - 1, (void *)json, MHD_RESPMEM_PERSISTENT);
  enum MHD_Result ret;

  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json");
  ret = websrv_queue_response(conn, MHD_HTTP_FORBIDDEN, resp);
  MHD_destroy_response(resp);
  return ret;
}

static enum MHD_Result
websrv_body_too_large(struct MHD_Connection *conn) {
  static const char json[] =
    "{\"ok\":false,\"error\":\"request body is too large\","
    "\"error_code\":\"request_body_too_large\",\"error_arg\":\"\"}";
  struct MHD_Response *resp =
    MHD_create_response_from_buffer(sizeof(json) - 1, (void *)json,
                                    MHD_RESPMEM_PERSISTENT);
  enum MHD_Result ret;

  if(!resp) {
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json");
  ret = websrv_queue_response(conn, MHD_HTTP_CONTENT_TOO_LARGE, resp);
  MHD_destroy_response(resp);
  return ret;
}

enum MHD_Result
websrv_queue_response(struct MHD_Connection *conn, unsigned int status,
                      struct MHD_Response *resp) {
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  return MHD_queue_response(conn, status, resp);
}

void
websrv_stop(void) {
  g_stop_requested = 1;
  if(g_listen_fd >= 0) {
    shutdown(g_listen_fd, SHUT_RDWR);
  }
}

int
websrv_stop_requested(void) {
  return g_stop_requested;
}

static enum MHD_Result
websrv_on_request(void *cls, struct MHD_Connection *conn, const char *url,
                  const char *method, const char *version,
                  const char *upload_data, size_t *upload_data_size,
                  void **con_cls) {
  request_context_t *ctx = *con_cls;
  (void)cls;
  (void)version;

  if(strcmp(method, MHD_HTTP_METHOD_GET) &&
     strcmp(method, MHD_HTTP_METHOD_POST) &&
     strcmp(method, MHD_HTTP_METHOD_HEAD)) {
    return MHD_NO;
  }

  if(!ctx) {
    if(!(ctx = calloc(1, sizeof(*ctx)))) {
      return MHD_NO;
    }
    ctx->upload_stream = !strcmp(url, "/api/upload-file") &&
                         !strcmp(method, MHD_HTTP_METHOD_POST);
    ctx->authorized = strncmp(url, "/api/", 5) != 0;
#ifdef __SCE__
    /* Match the upstream PS5 Web File Manager: the local PS5 UI/API does not
       require a browser-supplied token. Host builds retain token protection
       for regression and security testing. */
    if(!strncmp(url, "/api/", 5)) {
      ctx->authorized = 1;
    }
#endif
    *con_cls = ctx;
    return MHD_YES;
  }

  if(!ctx->authorized && !strncmp(url, "/api/", 5)) {
    ctx->authorized = websrv_authorized(conn);
  }
  if(!ctx->authorized) {
    *upload_data_size = 0;
    return websrv_access_denied(conn);
  }

  if(*upload_data_size) {
    size_t chunk_size = *upload_data_size;

    if(ctx->upload_stream) {
      if(!ctx->upload_ctx && filemgr_upload_begin(conn, &ctx->upload_ctx)) {
        ctx->too_large = 1;
      }
      if(ctx->upload_ctx && filemgr_upload_data(ctx->upload_ctx, upload_data,
                                                chunk_size)) {
        ctx->too_large = 1;
      }
      *upload_data_size = 0;
      return MHD_YES;
    }

    if(chunk_size > REQUEST_BODY_MAX - ctx->size) {
      ctx->too_large = 1;
    } else if(!ctx->too_large) {
      char *body = realloc(ctx->body, ctx->size + chunk_size + 1);
      if(!body) {
        return MHD_NO;
      }
      ctx->body = body;
      memcpy(ctx->body + ctx->size, upload_data, chunk_size);
      ctx->size += chunk_size;
      ctx->body[ctx->size] = 0;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  if(ctx->too_large) {
    return ctx->upload_stream ?
      filemgr_upload_finish(conn, ctx->upload_ctx) :
      websrv_body_too_large(conn);
  }

  if(ctx->upload_stream) {
    if(!ctx->upload_ctx && filemgr_upload_begin(conn, &ctx->upload_ctx)) {
      return filemgr_upload_finish(conn, ctx->upload_ctx);
    }
    return filemgr_upload_finish(conn, ctx->upload_ctx);
  }

  if(!strncmp(url, "/api/", 5)) {
    return filemgr_api_request(conn, url, method, ctx->body, ctx->size);
  }
  if(!strcmp(url, "/fs")) {
    return filemgr_fs_request(conn);
  }
  if(!strcmp(url, "/") || !url[0]) {
    return asset_request(conn, "/index.html");
  }
  if(!strcmp(url, "/favicon.ico")) {
    return asset_request(conn, "/icon0.png");
  }
  return asset_request(conn, url);
}

static void
websrv_on_completed(void *cls, struct MHD_Connection *connection,
                    void **con_cls, enum MHD_RequestTerminationCode toe) {
  (void)cls;
  (void)connection;
  (void)toe;
  request_context_t *ctx = *con_cls;

  if(ctx) {
    if(ctx->upload_ctx) {
      filemgr_upload_free(ctx->upload_ctx);
    }
    free(ctx->body);
    free(ctx);
  }
  *con_cls = NULL;
}

int
websrv_listen(unsigned short port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  struct MHD_Daemon *httpd;
  socklen_t addr_len;
  int connfd;
  int srvfd;

  signal(SIGPIPE, SIG_IGN);

  if((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(srvfd);
    return -1;
  }
  {
    const int rcvbuf = HTTP_SOCKET_RCVBUF_SIZE;

    /* Set before listen so accepted PS5 sockets inherit the larger window. */
    (void)setsockopt(srvfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
    perror("bind");
    close(srvfd);
    return -1;
  }
  if(listen(srvfd, 16)) {
    perror("listen");
    close(srvfd);
    return -1;
  }
  {
    struct sockaddr_in bound_addr = {0};
    socklen_t bound_len = sizeof(bound_addr);
    if(getsockname(srvfd, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
      perror("getsockname");
      close(srvfd);
      return -1;
    }
    port = ntohs(bound_addr.sin_port);
  }
  g_stop_requested = 0;
  g_listen_fd = srvfd;

  if(!(httpd = MHD_start_daemon(MHD_USE_ITC |
                                MHD_USE_NO_LISTEN_SOCKET | MHD_USE_DEBUG |
                                MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_TURBO,
                                0, NULL, NULL, &websrv_on_request, NULL,
                                MHD_OPTION_CONNECTION_MEMORY_LIMIT,
                                (size_t)HTTP_CONNECTION_MEMORY_LIMIT,
                                MHD_OPTION_CONNECTION_MEMORY_INCREMENT,
                                (size_t)HTTP_CONNECTION_MEMORY_INCREMENT,
                                MHD_OPTION_CONNECTION_LIMIT,
                                (unsigned int)HTTP_CONNECTION_LIMIT,
                                MHD_OPTION_PER_IP_CONNECTION_LIMIT,
                                (unsigned int)HTTP_PER_IP_CONNECTION_LIMIT,
                                MHD_OPTION_CONNECTION_TIMEOUT,
                                (unsigned int)HTTP_CONNECTION_TIMEOUT_SECONDS,
                                MHD_OPTION_NOTIFY_COMPLETED,
                                &websrv_on_completed, NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    close(srvfd);
    return -1;
  }

  printf("listening on port %u\n", (unsigned int)port);
  if(g_ready_callback) {
    g_ready_callback(port, g_ready_callback_arg);
  }

  while(!g_stop_requested) {
    addr_len = sizeof(client_addr);
    if((connfd = accept(srvfd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
      if(errno == EINTR) {
        continue;
      }
      if(errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      if(errno == EMFILE || errno == ENFILE) {
        /* Do not tear down active MHD connections or filesystem workers merely
         * because accepts are temporarily exhausted. */
        if(!g_stop_requested) {
          fprintf(stderr, "accept temporarily out of file descriptors\n");
          usleep(100000);
        }
        continue;
      }
      if(websrv_accept_error_retryable(errno)) {
        if(!g_stop_requested) {
          fprintf(stderr, "accept temporarily unavailable: %s\n",
                  strerror(errno));
          usleep(100000);
        }
        continue;
      }
      if(!g_stop_requested) perror("accept");
      break;
    }
    websrv_tune_connection_socket(connfd);
    if(MHD_add_connection(httpd, connfd, (struct sockaddr *)&client_addr,
                          addr_len) != MHD_YES) {
      /* A rejected peer (for example, a client exceeding the bounded MHD
       * connection limit) must not tear down the listener or cancel a long
       * filesystem job. Drop only that socket and keep serving existing work. */
      fprintf(stderr, "MHD_add_connection rejected client; keeping server alive\n");
      close(connfd);
      continue;
    }
  }

  MHD_stop_daemon(httpd);
  g_listen_fd = -1;
  return close(srvfd);
}
