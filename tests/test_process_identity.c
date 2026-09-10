#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "process_identity.h"

static void
put_record(uint8_t *record, size_t size, int32_t structsize, pid_t pid,
           const char *name) {
  memset(record, 0, size);
  memcpy(record, &structsize, sizeof(structsize));
  memcpy(record + PS5_KINFO_PROC_PID_OFFSET, &pid, sizeof(pid));
  memcpy(record + PS5_KINFO_PROC_TDNAME_OFFSET, name, strlen(name) + 1);
}

int
main(void) {
  static const char service_name[] = "mkpfs-svc-7c91";
  uint8_t record[512];
  pid_t pid;
  size_t record_size;

  put_record(record, sizeof(record), sizeof(record), 321, service_name);
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == 1);
  assert(pid == 321);
  assert(record_size == sizeof(record));

  put_record(record, sizeof(record), sizeof(record), 322, "other-service");
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == 0);
  assert(pid == 322);

  put_record(record, sizeof(record), sizeof(record), 323, "mkpfs-svc-7c9");
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == 0);

  put_record(record, sizeof(record), 0, 324, service_name);
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == -1);

  put_record(record, sizeof(record),
             PS5_KINFO_PROC_TDNAME_OFFSET + PS5_KINFO_PROC_TDNAME_BYTES - 1,
             325, service_name);
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == -1);

  put_record(record, sizeof(record), sizeof(record) + 1, 326, service_name);
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == -1);

  put_record(record, sizeof(record), sizeof(record), 327, service_name);
  memset(record + PS5_KINFO_PROC_TDNAME_OFFSET, 'x',
         PS5_KINFO_PROC_TDNAME_BYTES);
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == -1);

  put_record(record, sizeof(record), sizeof(record), 0, service_name);
  assert(ps5_kinfo_proc_match(record, sizeof(record), service_name, &pid,
                               &record_size) == -1);

  assert(ps5_kinfo_proc_match(record,
                               PS5_KINFO_PROC_TDNAME_OFFSET +
                               PS5_KINFO_PROC_TDNAME_BYTES - 1,
                               service_name, &pid, &record_size) == -1);
  assert(ps5_kinfo_proc_match(record, sizeof(record),
                               "12345678901234567", &pid,
                               &record_size) == -1);

  puts("PROCESS_IDENTITY_TESTS_PASS");
  return 0;
}
