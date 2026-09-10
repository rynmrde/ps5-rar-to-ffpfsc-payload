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
#include <sys/sysctl.h>
#endif

#include "app_installer.h"
#include "filemgr.h"
#include "notify.h"
#include "process_identity.h"
#include "websrv.h"

#define PROCESS_NAME "rar-to-ffpfsc-ps5-payload.elf"
#define SERVICE_PROCESS_NAME "mkpfs-svc-7c91"
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
static pthread_mutex_t g_launcher_install_lock = PTHREAD_MUTEX_INITIALIZER;

/* A reload replaces a previous payload instance rather than leaving a stale
 * listener on 8888 and silently moving this instance to a fallback.  The
 * exported ki_tdname region is 16 characters plus NUL, so the service token
 * deliberately fits it and is not the longer ELF/display filename. */
static pid_t
find_pid(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }
  if(!(buf = malloc(buf_size))) {
    perror("malloc");
    return -1;
  }
  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) {
    perror("sysctl");
    free(buf);
    return -1;
  }

  for(size_t offset = 0; offset < buf_size;) {
    pid_t record_pid;
    size_t record_size;
    int match = ps5_kinfo_proc_match(buf + offset, buf_size - offset, name,
                                     &record_pid, &record_size);

    if(match < 0) {
      fprintf(stderr, "invalid KERN_PROC record; stale payload cleanup skipped\n");
      pid = -1;
      break;
    }
    offset += record_size;
    if(match && record_pid != mypid) {
      /* The parser returns only an exact match for the dedicated service
       * token from a complete, validated record. */
      pid = record_pid;
    }
  }

  free(buf);
  return pid;
}

static int
retire_stale_payloads(void) {
  pid_t pid;

  while((pid = find_pid(SERVICE_PROCESS_NAME)) > 0) {
    if(kill(pid, SIGKILL)) {
      perror("kill");
      return -1;
    }
    sleep(1);
  }
  return 0;
}

static void *
launcher_install_worker(void *arg) {
  unsigned short port = (unsigned short)(uintptr_t)arg;

  pthread_mutex_lock(&g_launcher_install_lock);
  if(app_install_if_needed(port)) {
    fputs("launcher installation failed; server remains available\n", stderr);
  }
  pthread_mutex_unlock(&g_launcher_install_lock);
  return NULL;
}

static void
server_ready(unsigned short port, void *arg) {
  pthread_t launcher_thread;

  (void)arg;
  /* MHD has already started on this exact port. */
  notify_user("RAR to FFPFSC PS5 Payload\nVersion: %s\nPort: %u",
              VERSION_TAG, port);

  /* AppInstUtil work can take time on physical hardware.  It must not block
   * the proven listener/accept loop or turn a healthy HTTP daemon into an
   * apparently unreachable payload. */
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
  unsigned short start_port;
  int listen_result;
#ifndef __SCE__
  const char *host_token = getenv("WFM_ACCESS_TOKEN");
#endif

  (void)argc;
  (void)argv;

#ifdef __SCE__
  syscall(SYS_thr_set_name, -1, SERVICE_PROCESS_NAME);
  if(retire_stale_payloads()) {
    return 1;
  }
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
  app_register_assets();
  websrv_set_ready_callback(server_ready, NULL);
#endif

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  if(filemgr_resume_interrupted_conversions() < 0) {
    fputs("interrupted conversion recovery scan failed; server remains available\n",
          stderr);
  }

  /* Start from 8888 and fall back only after a real bind(2) reports that the
   * current port is occupied.  Unlike a probe-then-bind scheme, this has no
   * race window; websrv_listen passes the actual bound port to server_ready. */
  start_port = configured_port();
  port = start_port;
  while(1) {
    listen_result = websrv_listen(port);
    if(websrv_stop_requested()) {
      break;
    }
    if(listen_result < 0 && errno == EADDRINUSE && port < 65535u) {
      port++;
      continue;
    }
    port = start_port;
    sleep(3);
  }

  return 0;
}
