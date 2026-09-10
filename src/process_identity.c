#include "process_identity.h"

#include <stddef.h>
#include <string.h>

#ifdef __SCE__
#include <sys/user.h>

_Static_assert(offsetof(struct kinfo_proc, ki_structsize) == 0,
               "unexpected ki_structsize offset");
_Static_assert(sizeof(((struct kinfo_proc *)0)->ki_structsize) == sizeof(int),
               "unexpected ki_structsize type");
_Static_assert(offsetof(struct kinfo_proc, ki_pid) ==
               PS5_KINFO_PROC_PID_OFFSET, "unexpected ki_pid offset");
_Static_assert(sizeof(((struct kinfo_proc *)0)->ki_pid) == sizeof(pid_t),
               "unexpected ki_pid type");
_Static_assert(offsetof(struct kinfo_proc, ki_tdname) ==
               PS5_KINFO_PROC_TDNAME_OFFSET, "unexpected ki_tdname offset");
_Static_assert(sizeof(((struct kinfo_proc *)0)->ki_tdname) ==
               PS5_KINFO_PROC_TDNAME_BYTES, "unexpected ki_tdname size");
#endif

int
ps5_kinfo_proc_match(const uint8_t *record, size_t available,
                     const char *service_name, pid_t *pid,
                     size_t *record_size) {
  int structsize;
  pid_t record_pid;
  size_t name_len;

  if(!record || !service_name || !pid || !record_size ||
     available < PS5_KINFO_PROC_TDNAME_OFFSET + PS5_KINFO_PROC_TDNAME_BYTES) {
    return -1;
  }
  name_len = strnlen(service_name, PS5_KINFO_PROC_TDNAME_BYTES);
  if(!name_len || name_len == PS5_KINFO_PROC_TDNAME_BYTES) {
    return -1;
  }
  memcpy(&structsize, record, sizeof(structsize));
  if(structsize <= 0 || (size_t)structsize < PS5_KINFO_PROC_TDNAME_OFFSET +
     PS5_KINFO_PROC_TDNAME_BYTES || (size_t)structsize > available) {
    return -1;
  }
  memcpy(&record_pid, record + PS5_KINFO_PROC_PID_OFFSET, sizeof(record_pid));
  if(record_pid <= 0 ||
     memchr(record + PS5_KINFO_PROC_TDNAME_OFFSET, '\0',
            PS5_KINFO_PROC_TDNAME_BYTES) == NULL) {
    return -1;
  }
  *pid = record_pid;
  *record_size = (size_t)structsize;
  return strcmp(service_name,
                (const char *)(record + PS5_KINFO_PROC_TDNAME_OFFSET)) == 0;
}
