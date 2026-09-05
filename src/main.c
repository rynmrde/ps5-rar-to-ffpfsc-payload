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
#endif

#include "app_installer.h"
#include "notify.h"
#include "websrv.h"

#define PROCESS_NAME "web-file-mgr.elf"
#define DEFAULT_PORT 8888

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
#ifndef __SCE__
  const char *host_token = getenv("WFM_ACCESS_TOKEN");
#endif
#ifdef __SCE__
  unsigned short notified_port = 0;
#endif

  (void)argc;
  (void)argv;

#ifdef __SCE__
  syscall(SYS_thr_set_name, -1, PROCESS_NAME);
#endif

  puts(PROCESS_NAME);
  printf("version: %s\n", VERSION_TAG);

#ifndef __SCE__
  if(host_token && websrv_set_access_token(host_token)) {
    fputs("invalid host HTTP access token\n", stderr);
    return 1;
  }
#endif

#ifdef __SCE__
  app_install_if_needed();
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
      notify_user("Web File Manager\nVersion: %s\nPort: %u", VERSION_TAG, port);
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
