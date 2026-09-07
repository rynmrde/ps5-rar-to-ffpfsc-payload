#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "archive_extract.h"

static unsigned int last_percent;

static void progress(unsigned int percent, const char *current, void *opaque) {
  (void)current;
  (void)opaque;
  if(percent > last_percent) last_percent = percent;
}

static int read_equals(const char *path, const char *expected) {
  char buffer[256];
  int fd = open(path, O_RDONLY);
  ssize_t got;
  if(fd < 0) return -1;
  got = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if(got < 0) return -1;
  buffer[got] = 0;
  return strcmp(buffer, expected) == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
  volatile int cancel = 0;
  char error[160] = {0};
  char output[4096];
  int rc;

  if(argc != 5 && argc != 6) {
    fprintf(stderr, "usage: %s ARCHIVE DEST RELATIVE-PATH CONTENT [PASSWORD]\n", argv[0]);
    return 2;
  }
  if(mkdir(argv[2], 0700) && errno != EEXIST) {
    perror("mkdir destination");
    return 2;
  }
  rc = archive_extract_run(argv[1], argv[2], argc == 6 ? argv[5] : "", 1, &cancel, progress, NULL,
                           error, sizeof(error));
  if(rc != 0) {
    fprintf(stderr, "extraction failed: rc=%d error=%s\n", rc, error);
    return 1;
  }
  if(snprintf(output, sizeof(output), "%s/%s", argv[2], argv[3]) >= (int)sizeof(output) ||
     read_equals(output, argv[4])) {
    fprintf(stderr, "output mismatch: %s\n", output);
    return 1;
  }

  cancel = 1;
  error[0] = 0;
  rc = archive_extract_run(argv[1], argv[2], argc == 6 ? argv[5] : "", 1, &cancel, progress, NULL,
                           error, sizeof(error));
  if(rc != 255 || strcmp(error, "canceled")) {
    fprintf(stderr, "pre-cancel was not honored: rc=%d error=%s\n", rc, error);
    return 1;
  }
  return 0;
}
