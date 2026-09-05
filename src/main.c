#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __SCE__
#include <sys/syscall.h>
#include <sys/sysctl.h>
#endif

#include "app_installer.h"
#include "notify.h"
#include "websrv.h"

#define PROCESS_NAME "web-file-mgr.elf"
#define DEFAULT_PORT 8888
#define ACCESS_TOKEN_BYTES 16

static int
configure_access_token(char *token, size_t token_size) {
  static const char hex[] = "0123456789abcdef";
  unsigned char bytes[ACCESS_TOKEN_BYTES];
  const char *configured = getenv("WFM_ACCESS_TOKEN");

  if(configured && strlen(configured) >= ACCESS_TOKEN_BYTES * 2 &&
     strlen(configured) < token_size) {
    memcpy(token, configured, strlen(configured) + 1);
    return websrv_set_access_token(token);
  }
#ifdef __SCE__
  for(size_t i = 0; i < sizeof(bytes); i += sizeof(uint32_t)) {
    uint32_t random_value = arc4random();
    memcpy(bytes + i, &random_value, sizeof(random_value));
  }
#else
  FILE *random = fopen("/dev/urandom", "rb");
  if(!random || fread(bytes, 1, sizeof(bytes), random) != sizeof(bytes)) {
    if(random) fclose(random);
    return -1;
  }
  fclose(random);
#endif
  if(token_size < sizeof(bytes) * 2 + 1) return -1;
  for(size_t i = 0; i < sizeof(bytes); i++) {
    token[i * 2] = hex[bytes[i] >> 4];
    token[i * 2 + 1] = hex[bytes[i] & 0x0f];
  }
  token[sizeof(bytes) * 2] = 0;
  return websrv_set_access_token(token);
}

static int
port_available(unsigned short port) {
  struct sockaddr_in addr;
  int fd;
  int ret;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }
  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(fd);
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  ret = !bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  close(fd);
  return ret;
}

static unsigned short
find_available_port(unsigned short start) {
  unsigned int port;

  for(port = start; port <= 65535; port++) {
    int available = port_available((unsigned short)port);
    if(available < 0) {
      return 0;
    }
    if(available) {
      return (unsigned short)port;
    }
  }
  return 0;
}

int
main(int argc, char **argv) {
  unsigned short port;
  char access_token[ACCESS_TOKEN_BYTES * 2 + 1];
#ifdef __SCE__
  unsigned short notified_port = 0;
  int install_launcher = argc > 1 && !strcmp(argv[1], "--install-launcher");
#endif

#ifdef __SCE__
  syscall(SYS_thr_set_name, -1, PROCESS_NAME);
#endif

  puts(PROCESS_NAME);
  printf("version: %s\n", VERSION_TAG);
  if(configure_access_token(access_token, sizeof(access_token))) {
    fputs("could not generate HTTP access token\n", stderr);
    return 1;
  }

#ifdef __SCE__
  if(install_launcher && app_install_if_needed()) {
    fputs("launcher installation failed\n", stderr);
  }
#else
  (void)argc;
  (void)argv;
#endif

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  while(1) {
    port = find_available_port(DEFAULT_PORT);
    if(!port) {
      fprintf(stderr, "no available port from %u\n", DEFAULT_PORT);
      sleep(3);
      continue;
    }

    printf("listening on port %u\n", port);
#ifdef __SCE__
    if(notified_port != port) {
      notify_user("Web File Manager\nVersion: %s\nPort: %u\nToken: %s", VERSION_TAG, port, access_token);
      notified_port = port;
    }
#endif

    websrv_listen(port);
    if(websrv_stop_requested()) {
      break;
    }
    sleep(3);
  }

  return 0;
}
