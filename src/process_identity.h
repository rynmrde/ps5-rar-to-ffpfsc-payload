#ifndef PROCESS_IDENTITY_H
#define PROCESS_IDENTITY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * PS5 KERN_PROC_PROC records use the target SDK ABI offsets below.  The parser
 * treats the sysctl response as untrusted bytes and validates every record
 * before accessing a field.  ki_tdname contains at most 16 characters plus
 * its terminating NUL; service identities used with this parser must fit.
 */
enum {
  PS5_KINFO_PROC_PID_OFFSET = 72,
  PS5_KINFO_PROC_TDNAME_OFFSET = 394,
  PS5_KINFO_PROC_TDNAME_BYTES = 17
};

/*
 * Return 1 for an exact thread-name match, 0 for a valid non-match, and -1
 * for an invalid or truncated record.  On a valid record, record_size receives
 * the validated size used to advance the KERN_PROC buffer.
 */
int ps5_kinfo_proc_match(const uint8_t *record, size_t available,
                         const char *service_name, pid_t *pid,
                         size_t *record_size);

#endif
