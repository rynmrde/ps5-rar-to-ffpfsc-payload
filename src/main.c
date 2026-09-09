#include <errno.h>
#include <pthread.h>
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
#include "filemgr.h"
#include "notify.h"
#include "websrv.h"

#define PROCESS_NAME "mkpfs-ps5.elf"
#define DEFAULT_PORT 8888

static unsigned short
configured_port(void) {
  const char *value = getenv("WFM_PORT");
  char *end;
  unsigned long port;
  if(!value || !*value) return DEFAULT_PORT;
  port = strtoul(value, &end, 10);
  if(*end || !port || port > 65535u) return DEFAULT_PORT;
  return (unsigned short)port;
}

#ifdef __SCE__
static void
*launcher_install_worker(void *arg) {
  unsigned short port = (unsigned short)(uintptr_t)arg;

  if(app_install_if_needed(port)) {
    fputs("launcher installation failed; server remains available\n", stderr);
  }
  return NULL;
}

static void
server_ready(unsigned short port, void *arg) {
  pthread_t launcher_thread;
  (void)arg;

  /* The listener and MHD are ready before this notification is submitted.
   * Installing Home Screen metadata can take noticeably longer on some
   * firmware, so it cannot be allowed to make a healthy web server look dead.
   */
  notify_user("MkPFS-PS5\nVersion: %s\nPort: %u", VERSION_TAG, port);
  if(pthread_create(&launcher_thread, NULL, launcher_install_worker,
                    (void *)(uintptr_t)port)) {
    fputs("launcher installation worker creation failed; server remains available\n",
          stderr);
  } else {
    pthread_detach(launcher_thread);
  }
}
#endif

int
main(int argc, char **argv) {
  unsigned short port;
  unsigned short configured;
  int listen_result;
#ifndef __SCE__
  const char *host_token = getenv("WFM_ACCESS_TOKEN");
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
  /* Register the favicon before MHD can accept a launcher request. */
  app_register_assets();
  websrv_set_ready_callback(server_ready, NULL);
#endif

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  if(filemgr_resume_interrupted_conversions() < 0) {
    fputs("interrupted conversion recovery scan failed; server remains available\n",
          stderr);
  }
  if(filemgr_resume_interrupted_downloads() < 0) {
    fputs("interrupted download recovery scan failed; server remains available\n",
          stderr);
  }

  configured = configured_port();
  port = configured;
  while(1) {
    listen_result = websrv_listen(port);
    if(websrv_stop_requested()) {
      break;
    }
    if(listen_result == -1 && errno == EADDRINUSE && port < 65535u) {
      /* Preserve the upstream automatic fallback convention: 8888, 8889,
       * 8890, ... . The successful bound port is the one passed to the
       * readiness callback, notification, and launcher metadata. */
      port++;
      continue;
    }
    port = configured;
    sleep(3);
  }

  return 0;
}
