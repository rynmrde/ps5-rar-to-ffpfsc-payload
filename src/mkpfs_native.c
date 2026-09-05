#define _GNU_SOURCE
#include "mkpfs_native.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define PFSC_MAGIC 0x43534650u
#define PFSC_UNK4 0
#define PFSC_UNK8 6
#define PFSC_BLOCK_SIZE 0x10000u
#define PFSC_HEADER_SIZE 0x30u
#define PFSC_OFFSETS_OFFSET 0x400u
#define PFSC_INITIAL_DATA_OFFSET 0x10000u

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static int write_all(FILE *f, const void *data, size_t size) {
  return fwrite(data, 1, size, f) == size ? 0 : EIO;
}

static int read_all(FILE *f, void *data, size_t size) {
  return fread(data, 1, size, f) == size ? 0 : EIO;
}

static void put_u32le(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put_u64le(unsigned char *p, uint64_t v) {
  for (unsigned int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t get_u32le(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64le(const unsigned char *p) {
  uint64_t v = 0;
  for (unsigned int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

static int append_component(char *out, size_t cap, size_t *used,
                            const char *component) {
  size_t n = strlen(component);
  if (!n || !strcmp(component, ".") || !strcmp(component, "..")) return EINVAL;
  if (strchr(component, '\\')) return EINVAL;
  if (*used + n + 1 >= cap) return ENAMETOOLONG;
  if (*used > 1) out[(*used)++] = '/';
  memcpy(out + *used, component, n);
  *used += n; out[*used] = '\0';
  return 0;
}

int mkpfs_normalize_path(const char *input, char *output, size_t output_size) {
  const char *p; size_t used = 0;
  if (!input || !output || output_size < 2 || input[0] != '/') return EINVAL;
  output[used++] = '/'; output[used] = '\0'; p = input + 1;
  while (*p) {
    const char *start = p; size_t n; char component[NAME_MAX + 1];
    while (*p && *p != '/') p++;
    n = (size_t)(p - start);
    if (n > NAME_MAX) return ENAMETOOLONG;
    if (n) { memcpy(component, start, n); component[n] = '\0'; if (append_component(output, output_size, &used, component)) return EINVAL; }
    while (*p == '/') p++;
  }
  return 0;
}

static int scan_dir(const char *path, mkpfs_scan_result_t *result) {
  DIR *dir = opendir(path); struct dirent *entry;
  if (!dir) return errno;
  while ((entry = readdir(dir)) != NULL) {
    char child[PATH_MAX]; struct stat st; int rc;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) { closedir(dir); return ENAMETOOLONG; }
    if (lstat(child, &st) != 0) { rc = errno; closedir(dir); return rc; }
    if (S_ISREG(st.st_mode)) { result->file_count++; result->total_bytes += (uint64_t)st.st_size; }
    else if (S_ISDIR(st.st_mode)) { result->directory_count++; rc = scan_dir(child, result); if (rc) { closedir(dir); return rc; } }
    else if (S_ISLNK(st.st_mode)) { closedir(dir); return ELOOP; }
  }
  closedir(dir); return 0;
}

int mkpfs_scan_folder(const char *root, mkpfs_scan_result_t *result) {
  struct stat st;
  if (!root || !result || lstat(root, &st) != 0) return errno;
  if (!S_ISDIR(st.st_mode)) return ENOTDIR;
  memset(result, 0, sizeof(*result)); return scan_dir(root, result);
}

static int write_zeros(FILE *out, uint64_t count) {
  unsigned char zeros[4096] = {0};
  while (count) { size_t n = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count; if (write_all(out, zeros, n)) return EIO; count -= n; }
  return 0;
}

static int cleanup_pack(FILE *in, FILE *out, const char *temp_path, int error) {
  if (in) fclose(in);
  if (out) fclose(out);
  if (temp_path) unlink(temp_path);
  return error;
}

int mkpfs_pack_pfsc_file(const char *input_path, const char *output_path,
                         int compression_level, volatile int *cancel_requested,
                         mkpfs_progress_callback progress, void *opaque) {
  struct stat st; FILE *in = NULL, *out = NULL; char temp_path[PATH_MAX];
  unsigned char *raw = NULL, *compressed = NULL; uint64_t *offsets = NULL;
  uint64_t logical_size, block_count, offsets_bytes, data_offset, stored_pos; int rc = 0;
  if (!input_path || !output_path || stat(input_path, &st) != 0) return errno;
  if (!S_ISREG(st.st_mode) || compression_level < 0 || compression_level > 9) return EINVAL;
  logical_size = align_up_u64((uint64_t)st.st_size, PFSC_BLOCK_SIZE); if (!logical_size) logical_size = PFSC_BLOCK_SIZE;
  block_count = logical_size / PFSC_BLOCK_SIZE;
  if (block_count > (SIZE_MAX / sizeof(*offsets)) - 1) return EOVERFLOW;
  offsets_bytes = (block_count + 1) * sizeof(*offsets);
  data_offset = align_up_u64(PFSC_OFFSETS_OFFSET + offsets_bytes, PFSC_INITIAL_DATA_OFFSET);
  if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", output_path, (long)getpid()) >= (int)sizeof(temp_path)) return ENAMETOOLONG;
  in = fopen(input_path, "rb"); if (!in) return errno;
  out = fopen(temp_path, "wb+"); if (!out) return cleanup_pack(in, NULL, temp_path, errno);
  offsets = (uint64_t *)calloc((size_t)(block_count + 1), sizeof(*offsets)); raw = (unsigned char *)calloc(1, PFSC_BLOCK_SIZE); compressed = (unsigned char *)malloc(compressBound(PFSC_BLOCK_SIZE));
  if (!offsets || !raw || !compressed) { free(offsets); free(raw); free(compressed); return cleanup_pack(in, out, temp_path, ENOMEM); }
  unsigned char header[PFSC_HEADER_SIZE] = {0};
  put_u32le(header + 0x00, PFSC_MAGIC); put_u32le(header + 0x04, PFSC_UNK4); put_u32le(header + 0x08, PFSC_UNK8); put_u32le(header + 0x0c, PFSC_BLOCK_SIZE); put_u64le(header + 0x10, PFSC_BLOCK_SIZE); put_u64le(header + 0x18, PFSC_OFFSETS_OFFSET); put_u64le(header + 0x20, data_offset); put_u64le(header + 0x28, logical_size);
  if ((rc = write_all(out, header, sizeof(header))) || (rc = write_zeros(out, data_offset - sizeof(header)))) goto failed;
  stored_pos = data_offset;
  for (uint64_t i = 0; i < block_count; i++) {
    if (cancel_requested && *cancel_requested) { rc = ECANCELED; goto failed; }
    size_t got = fread(raw, 1, PFSC_BLOCK_SIZE, in); if (ferror(in)) { rc = EIO; goto failed; }
    if (got < PFSC_BLOCK_SIZE) memset(raw + got, 0, PFSC_BLOCK_SIZE - got);
    uLongf compressed_size = compressBound(PFSC_BLOCK_SIZE);
    if (compress2(compressed, &compressed_size, raw, PFSC_BLOCK_SIZE, compression_level) != Z_OK) { rc = EIO; goto failed; }
    offsets[i] = stored_pos;
    if (compressed_size < PFSC_BLOCK_SIZE) { rc = write_all(out, compressed, compressed_size); stored_pos += compressed_size; }
    else { rc = write_all(out, raw, PFSC_BLOCK_SIZE); stored_pos += PFSC_BLOCK_SIZE; }
    if (rc) goto failed;
    if (progress && progress((i + 1) * PFSC_BLOCK_SIZE > (uint64_t)st.st_size ? (uint64_t)st.st_size : (i + 1) * PFSC_BLOCK_SIZE, (uint64_t)st.st_size, "compress", input_path, opaque)) { rc = ECANCELED; goto failed; }
  }
  offsets[block_count] = stored_pos;
  if (fseeko(out, (off_t)PFSC_OFFSETS_OFFSET, SEEK_SET) != 0) { rc = EIO; goto failed; }
  for (uint64_t i = 0; i <= block_count; i++) { unsigned char b[8]; put_u64le(b, offsets[i]); if (write_all(out, b, 8)) { rc = EIO; goto failed; } }
  if (fflush(out) != 0) { rc = EIO; goto failed; }
  fclose(in); fclose(out); in = out = NULL;
  if (rename(temp_path, output_path) != 0) { rc = errno; unlink(temp_path); goto done; }
  rc = 0; goto done;
failed: rc = cleanup_pack(in, out, temp_path, rc ? rc : EIO); in = out = NULL;
done: free(offsets); free(raw); free(compressed); return rc;
}

int mkpfs_verify_pfsc_file(const char *path, uint64_t *logical_size_out, uint64_t *block_count_out) {
  FILE *f = NULL; unsigned char header[PFSC_HEADER_SIZE]; uint64_t logical_size, block_count, offsets_offset, data_offset, previous = 0; int rc = 0;
  if (!path) return EINVAL;
  f = fopen(path, "rb");
  if (!f) return errno;
  if (read_all(f, header, sizeof(header))) { fclose(f); return EINVAL; }
  if (get_u32le(header) != PFSC_MAGIC || get_u32le(header + 4) != PFSC_UNK4 || get_u32le(header + 8) != PFSC_UNK8 || get_u32le(header + 12) != PFSC_BLOCK_SIZE || get_u64le(header + 16) != PFSC_BLOCK_SIZE) { fclose(f); return EINVAL; }
  offsets_offset = get_u64le(header + 24); data_offset = get_u64le(header + 32); logical_size = get_u64le(header + 40);
  if (offsets_offset != PFSC_OFFSETS_OFFSET || data_offset < PFSC_INITIAL_DATA_OFFSET || logical_size == 0 || logical_size % PFSC_BLOCK_SIZE) { fclose(f); return EINVAL; }
  block_count = logical_size / PFSC_BLOCK_SIZE;
  if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return EIO; }
  off_t file_size = ftello(f); if (file_size < 0 || (uint64_t)file_size < data_offset) { fclose(f); return EINVAL; }
  unsigned char *raw = (unsigned char *)malloc(PFSC_BLOCK_SIZE); unsigned char *stored = (unsigned char *)malloc(compressBound(PFSC_BLOCK_SIZE));
  if (!raw || !stored) { free(raw); free(stored); fclose(f); return ENOMEM; }
  if (fseeko(f, (off_t)offsets_offset, SEEK_SET) != 0) { rc = EIO; goto verify_done; }
  uint64_t start = 0;
  for (uint64_t i = 0; i < block_count; i++) {
    unsigned char a[8], b[8]; if (read_all(f, a, 8)) { rc = EINVAL; goto verify_done; }
    if (read_all(f, b, 8)) { rc = EINVAL; goto verify_done; }
    start = get_u64le(a); uint64_t end = get_u64le(b);
    if (i == 0 && start != data_offset) { rc = EINVAL; goto verify_done; }
    if (start < previous || end < start || end > (uint64_t)file_size || end - start > PFSC_BLOCK_SIZE) { rc = EINVAL; goto verify_done; }
    uint64_t stored_size = end - start;
    if (fseeko(f, (off_t)start, SEEK_SET) != 0 || read_all(f, stored, (size_t)stored_size)) { rc = EIO; goto verify_done; }
    if (stored_size == PFSC_BLOCK_SIZE) memcpy(raw, stored, PFSC_BLOCK_SIZE);
    else { uLongf raw_size = PFSC_BLOCK_SIZE; if (uncompress(raw, &raw_size, stored, (uLong)stored_size) != Z_OK || raw_size != PFSC_BLOCK_SIZE) { rc = EINVAL; goto verify_done; } }
    previous = start;
    if (fseeko(f, (off_t)(offsets_offset + (i + 1) * 8), SEEK_SET) != 0) { rc = EIO; goto verify_done; }
  }
verify_done: free(raw); free(stored); fclose(f);
  if (!rc) { if (logical_size_out) *logical_size_out = logical_size; if (block_count_out) *block_count_out = block_count; }
  return rc;
}

static void put_i64le(unsigned char *p, int64_t v) { put_u64le(p, (uint64_t)v); }

static int pfs_hash_path(const char *name, uint32_t *hash_out) {
  char path[256];
  size_t n;
  uint32_t h = 0;
  if (!name || !hash_out) return EINVAL;
  n = strlen(name);
  if (n == 0 || n > 240 || snprintf(path, sizeof(path), "/%s", name) >= (int)sizeof(path)) return ENAMETOOLONG;
  for (size_t i = 0; i < n + 1; i++) {
    unsigned char c = (unsigned char)path[i];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - ('a' - 'A'));
    h = (uint32_t)(c + 31u * h);
  }
  *hash_out = h;
  return 0;
}

static size_t pfs_dirent(unsigned char *out, size_t cap, uint32_t ino,
                         int32_t type, const char *name) {
  size_t n = strlen(name), size = (n + 17 + 7) & ~(size_t)7;
  if (n > INT32_MAX || size > cap) return 0;
  put_u32le(out, ino); put_u32le(out + 4, (uint32_t)type); put_u32le(out + 8, (uint32_t)n); put_u32le(out + 12, (uint32_t)size);
  memcpy(out + 16, name, n); memset(out + 16 + n, 0, size - 16 - n);
  return size;
}

static void pfs_inode(unsigned char *out, uint16_t mode, uint16_t nlink,
                      uint32_t flags, uint64_t size, uint64_t size_compressed,
                      uint64_t blocks, int32_t first_block, int readonly) {
  memset(out, 0, 0xA8);
  put_u32le(out, ((uint32_t)nlink << 16) | mode);
  put_u32le(out + 4, flags);
  put_i64le(out + 8, (int64_t)size); put_i64le(out + 16, (int64_t)size_compressed);
  put_i64le(out + 24, 0); put_i64le(out + 32, 0); put_i64le(out + 40, 0); put_i64le(out + 48, 0);
  put_u32le(out + 56, 0); put_u32le(out + 60, 0); put_u32le(out + 64, 0); put_u32le(out + 68, 0);
  put_u32le(out + 72, 0); put_u32le(out + 76, 0); put_u64le(out + 80, 0); put_u64le(out + 88, 0); put_u32le(out + 96, (uint32_t)blocks);
  if (first_block >= 0) put_u32le(out + 100, (uint32_t)first_block);
  for (int i = 1; i < 12; i++) put_u32le(out + 100 + i * 4, readonly ? 0xffffffffu : 0);
  for (int i = 0; i < 5; i++) put_u32le(out + 148 + i * 4, readonly ? 0xffffffffu : 0);
}

static int copy_file_at(FILE *out, uint64_t offset, const char *path) {
  FILE *in = fopen(path, "rb"); unsigned char *buf = NULL; size_t n;
  if (!in) return errno;
  if (fseeko(out, (off_t)offset, SEEK_SET) != 0) { fclose(in); return EIO; }
  buf = (unsigned char *)malloc(1024 * 1024); if (!buf) { fclose(in); return ENOMEM; }
  while ((n = fread(buf, 1, 1024 * 1024, in)) != 0) if (write_all(out, buf, n)) { free(buf); fclose(in); return EIO; }
  int read_error = ferror(in) ? EIO : 0;
  free(buf); fclose(in); return read_error;
}

int mkpfs_wrap_exfat_file(const char *exfat_path, const char *output_path,
                          const char *inner_name, int compression_level,
                          volatile int *cancel_requested,
                          mkpfs_progress_callback progress, void *opaque) {
  struct stat st; char pfsc_path[PATH_MAX], temp_path[PATH_MAX]; FILE *out = NULL;
  unsigned char *inode_table = NULL, *root_dir = NULL; uint64_t pfsc_size, raw_size, pfsc_blocks, final_blocks;
  uint32_t hash; int rc;
  if (!exfat_path || !output_path || !inner_name || !*inner_name) return EINVAL;
  if (stat(exfat_path, &st) != 0 || !S_ISREG(st.st_mode)) return errno ? errno : EINVAL;
  if (snprintf(pfsc_path, sizeof(pfsc_path), "%s.pfsc.tmp.%ld", output_path, (long)getpid()) >= (int)sizeof(pfsc_path)) return ENAMETOOLONG;
  if (mkpfs_pack_pfsc_file(exfat_path, pfsc_path, compression_level, cancel_requested, progress, opaque) != 0) return EIO;
  if (stat(pfsc_path, &st) != 0) { unlink(pfsc_path); return errno; }
  pfsc_size = (uint64_t)st.st_size; raw_size = 0; pfsc_blocks = (pfsc_size + 65535) / 65536;
  if (mkpfs_verify_pfsc_file(pfsc_path, &raw_size, NULL) != 0) { unlink(pfsc_path); return EINVAL; }
  if (pfs_hash_path(inner_name, &hash) != 0) { unlink(pfsc_path); return EINVAL; }
  if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", output_path, (long)getpid()) >= (int)sizeof(temp_path)) { unlink(pfsc_path); return ENAMETOOLONG; }
  out = fopen(temp_path, "wb+"); if (!out) { unlink(pfsc_path); return errno; }
  inode_table = (unsigned char *)calloc(4, 0xA8); root_dir = (unsigned char *)calloc(1, 65536);
  if (!inode_table || !root_dir) { free(inode_table); free(root_dir); fclose(out); unlink(temp_path); unlink(pfsc_path); return ENOMEM; }
  size_t pos = 0;
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 0, 4, ".");
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 0, 5, "..");
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 1, 2, "flat_path_table");
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 2, 3, "uroot");
  unsigned char fpt[8]; put_u32le(fpt, hash); put_u32le(fpt + 4, 3);
  unsigned char uroot[65536]; memset(uroot, 0, sizeof(uroot)); pos = 0; pos += pfs_dirent(uroot + pos, sizeof(uroot) - pos, 2, 4, "."); pos += pfs_dirent(uroot + pos, sizeof(uroot) - pos, 2, 5, ".."); pos += pfs_dirent(uroot + pos, sizeof(uroot) - pos, 3, 2, inner_name);
  pfs_inode(inode_table + 0 * 0xA8, 0x4405, 1, 0x20010, 65536, 65536, 1, 2, 1);
  pfs_inode(inode_table + 1 * 0xA8, 0x8101, 1, 0x20010, 8, 8, 1, 3, 1);
  pfs_inode(inode_table + 2 * 0xA8, 0x4405, 3, 0x10, 65536, 65536, 1, 5, 1);
  pfs_inode(inode_table + 3 * 0xA8, 0x8101, 1, 0x11, pfsc_size, raw_size, pfsc_blocks, 6, 1);
  unsigned char header[65536] = {0};
  put_i64le(header + 0x00, 2); put_i64le(header + 0x08, 20130315); put_i64le(header + 0x10, 0); header[0x1a] = 1; put_u32le(header + 0x1c, 0x0008); put_u32le(header + 0x20, 65536); put_i64le(header + 0x28, 1); put_i64le(header + 0x30, 4); put_i64le(header + 0x38, 0); put_i64le(header + 0x40, 1); put_u32le(header + 0x368, 1);
  if (write_all(out, header, sizeof(header)) || write_all(out, inode_table, 4 * 0xA8)) { rc = EIO; goto wrap_failed; }
  if (fseeko(out, 2 * 65536, SEEK_SET) != 0 || write_all(out, root_dir, 65536) || fseeko(out, 3 * 65536, SEEK_SET) != 0 || write_all(out, fpt, sizeof(fpt)) || fseeko(out, 5 * 65536, SEEK_SET) != 0 || write_all(out, uroot, sizeof(uroot)) || copy_file_at(out, 6 * 65536, pfsc_path)) { rc = EIO; goto wrap_failed; }
  final_blocks = 6 + pfsc_blocks; put_i64le(header + 0x38, (int64_t)final_blocks); if (fseeko(out, 0, SEEK_SET) != 0 || write_all(out, header, sizeof(header)) || fflush(out) != 0) { rc = EIO; goto wrap_failed; }
  fclose(out); out = NULL; if (rename(temp_path, output_path) != 0) { rc = errno; goto wrap_failed_no_out; }
  unlink(pfsc_path); free(inode_table); free(root_dir); return 0;
wrap_failed: if (out) fclose(out);
wrap_failed_no_out: unlink(temp_path); unlink(pfsc_path); free(inode_table); free(root_dir); return rc;
}

int mkpfs_convert_folder(const char *source, const char *destination, const char *output_name, const mkpfs_native_options_t *options, volatile int *cancel_requested) {
  mkpfs_scan_result_t scan; char normalized[PATH_MAX];
  (void)destination; (void)output_name; (void)options; (void)cancel_requested;
  if (mkpfs_normalize_path(source, normalized, sizeof(normalized)) != 0) return EINVAL;
  if (mkpfs_scan_folder(normalized, &scan) != 0) return errno ? errno : EIO;
  return ENOTSUP;
}
