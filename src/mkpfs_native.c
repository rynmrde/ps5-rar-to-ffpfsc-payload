#define _GNU_SOURCE
#include "mkpfs_native.h"
#include "exfat_upcase.h"

#include <ctype.h>
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

static void put_u16le(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
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

typedef struct exfat_node {
  char *name;
  char *path;
  int is_dir;
  uint64_t size;
  struct exfat_node **children;
  size_t child_count;
  size_t child_capacity;
  uint32_t first_cluster;
  uint32_t cluster_count;
} exfat_node_t;

static void exfat_free_tree(exfat_node_t *node) {
  if (!node) return;
  for (size_t i = 0; i < node->child_count; i++) exfat_free_tree(node->children[i]);
  free(node->children); free(node->name); free(node->path); free(node);
}

static int exfat_name_cmp(const struct dirent **a, const struct dirent **b) {
  const unsigned char *pa = (const unsigned char *)(*a)->d_name;
  const unsigned char *pb = (const unsigned char *)(*b)->d_name;
  while (*pa && *pb) {
    int ca = tolower(*pa++), cb = tolower(*pb++);
    if (ca != cb) return ca - cb;
  }
  return *pa - *pb;
}

static int exfat_ignored_name(const char *name) {
  return !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".DS_Store") || !strcmp(name, "Thumbs.db");
}

static int exfat_add_child(exfat_node_t *parent, exfat_node_t *child) {
  if (parent->child_count == parent->child_capacity) {
    size_t next = parent->child_capacity ? parent->child_capacity * 2 : 8;
    exfat_node_t **items = (exfat_node_t **)realloc(parent->children, next * sizeof(*items));
    if (!items) return ENOMEM;
    parent->children = items; parent->child_capacity = next;
  }
  parent->children[parent->child_count++] = child;
  return 0;
}

static exfat_node_t *exfat_scan_tree(const char *path, const char *name, int root, int *error_out) {
  struct stat st;
  exfat_node_t *node;
  if (lstat(path, &st) != 0) { *error_out = errno; return NULL; }
  if (!root && S_ISLNK(st.st_mode)) { *error_out = ELOOP; return NULL; }
  if (!root && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { *error_out = EINVAL; return NULL; }
  node = (exfat_node_t *)calloc(1, sizeof(*node));
  if (!node) { *error_out = ENOMEM; return NULL; }
  node->name = strdup(name ? name : ""); node->path = strdup(path);
  node->is_dir = root || S_ISDIR(st.st_mode); node->size = node->is_dir ? 0 : (uint64_t)st.st_size;
  if (!node->name || !node->path) { exfat_free_tree(node); *error_out = ENOMEM; return NULL; }
  if (node->is_dir) {
    struct dirent **entries = NULL;
    int count = scandir(path, &entries, NULL, exfat_name_cmp);
    if (count < 0) { exfat_free_tree(node); *error_out = errno; return NULL; }
    for (int i = 0; i < count; i++) {
      if (!exfat_ignored_name(entries[i]->d_name)) {
        char child_path[PATH_MAX];
        exfat_node_t *child;
        if (snprintf(child_path, sizeof(child_path), "%s/%s", path, entries[i]->d_name) >= (int)sizeof(child_path)) {
          *error_out = ENAMETOOLONG; free(entries[i]); for (int j = i + 1; j < count; j++) free(entries[j]); free(entries); exfat_free_tree(node); return NULL;
        }
        child = exfat_scan_tree(child_path, entries[i]->d_name, 0, error_out);
        if (!child || exfat_add_child(node, child)) {
          if (child) exfat_free_tree(child);
          free(entries[i]); for (int j = i + 1; j < count; j++) free(entries[j]); free(entries); exfat_free_tree(node); return NULL;
        }
      }
      free(entries[i]);
    }
    free(entries);
  }
  return node;
}

static uint32_t exfat_ceil_clusters(uint64_t bytes) {
  return bytes ? (uint32_t)((bytes + 65535u) / 65536u) : 0;
}

static uint32_t exfat_directory_entries(const exfat_node_t *node, int root) {
  uint32_t count = root ? 3u : 0u;
  for (size_t i = 0; i < node->child_count; i++) count += 2u + (uint32_t)((strlen(node->children[i]->name) + 14u) / 15u);
  return count;
}

static uint32_t exfat_node_clusters(const exfat_node_t *node, int root) {
  if (node->is_dir) return (exfat_directory_entries(node, root) * 32u + 65535u) / 65536u ?: 1u;
  return exfat_ceil_clusters(node->size);
}

static uint32_t exfat_tree_clusters(const exfat_node_t *node, int root) {
  uint32_t total = exfat_node_clusters(node, root);
  for (size_t i = 0; i < node->child_count; i++) total += exfat_tree_clusters(node->children[i], 0);
  return total;
}

static void exfat_assign_clusters(exfat_node_t *node, int root, uint32_t *next_cluster) {
  node->cluster_count = exfat_node_clusters(node, root);
  if (node->cluster_count) { node->first_cluster = *next_cluster; *next_cluster += node->cluster_count; }
  for (size_t i = 0; i < node->child_count; i++) exfat_assign_clusters(node->children[i], 0, next_cluster);
}

static uint16_t exfat_name_hash(const char *name) {
  uint16_t h = 0;
  for (size_t i = 0; name[i]; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - ('a' - 'A'));
    h = (uint16_t)(((h << 15) | (h >> 1)) + c);
    h = (uint16_t)(((h << 15) | (h >> 1)) + 0);
  }
  return h;
}

static uint16_t exfat_entry_checksum(const unsigned char *entries, size_t length) {
  uint16_t checksum = 0;
  for (size_t i = 0; i < length; i++) {
    if (i == 2 || i == 3) continue;
    checksum = (uint16_t)(((checksum << 15) | (checksum >> 1)) + entries[i]);
  }
  return checksum;
}

static size_t exfat_file_entry_set(unsigned char *out, size_t capacity, const exfat_node_t *node) {
  size_t name_length = strlen(node->name);
  size_t name_entries = (name_length + 14u) / 15u;
  size_t total = (2u + name_entries) * 32u;
  uint32_t timestamp = ((2024u - 1980u) << 25) | (1u << 21) | (1u << 16);
  if (name_length == 0 || name_length > 255 || total > capacity) return 0;
  for (size_t i = 0; i < name_length; i++) if ((unsigned char)node->name[i] > 127) return 0;
  memset(out, 0, total); out[0] = 0x85; out[1] = (unsigned char)(1u + name_entries);
  put_u16le(out + 4, node->is_dir ? 0x10 : 0x20); put_u32le(out + 8, timestamp); put_u32le(out + 12, timestamp); put_u32le(out + 16, timestamp);
  unsigned char *stream = out + 32; stream[0] = 0xC0; stream[1] = node->first_cluster >= 2 ? 1 : 0; stream[3] = (unsigned char)name_length; put_u16le(stream + 4, exfat_name_hash(node->name)); put_u64le(stream + 8, node->is_dir ? (uint64_t)node->cluster_count * 65536u : node->size); put_u32le(stream + 20, node->first_cluster >= 2 ? node->first_cluster : 0); put_u64le(stream + 24, node->is_dir ? (uint64_t)node->cluster_count * 65536u : node->size);
  for (size_t i = 0; i < name_entries; i++) { unsigned char *entry = out + 64 + i * 32; entry[0] = 0xC1; for (size_t j = 0; j < 15; j++) { size_t at = i * 15 + j; if (at < name_length) put_u16le(entry + 2 + j * 2, (uint16_t)(unsigned char)node->name[at]); } }
  put_u16le(out + 2, exfat_entry_checksum(out, total));
  return total;
}

static int exfat_write_directory(FILE *out, const exfat_node_t *node, int root, uint32_t bitmap_clusters, uint32_t upcase_clusters, uint32_t cluster_count) {
  uint64_t capacity = (uint64_t)node->cluster_count * 65536u;
  uint64_t used = 0;
  unsigned char special[96];
  if (fseeko(out, (off_t)node->first_cluster * 65536, SEEK_SET) != 0) return EIO;
  if (root) {
    memset(special, 0, sizeof(special));
    special[0] = 0x83;
    special[32] = 0x81; put_u32le(special + 32 + 20, 2); put_u64le(special + 32 + 24, ((uint64_t)cluster_count + 7u) / 8u);
    special[64] = 0x82; put_u32le(special + 64 + 4, MKPFS_EXFAT_UPCASE_CHECKSUM); put_u32le(special + 64 + 20, 2 + bitmap_clusters); put_u64le(special + 64 + 24, MKPFS_EXFAT_UPCASE_SIZE);
    if (write_all(out, special, sizeof(special))) return EIO;
    used = sizeof(special);
  }
  for (size_t i = 0; i < node->child_count; i++) {
    unsigned char entry[32 * 20];
    size_t written = exfat_file_entry_set(entry, sizeof(entry), node->children[i]);
    if (!written || used + written > capacity || write_all(out, entry, written)) return EIO;
    used += written;
  }
  return used <= capacity ? write_zeros(out, capacity - used) : EIO;
}

static int exfat_write_directories(FILE *out, const exfat_node_t *node, int root, uint32_t bitmap_clusters, uint32_t upcase_clusters, uint32_t cluster_count) {
  for (size_t i = 0; i < node->child_count; i++) {
    const exfat_node_t *child = node->children[i];
    if (child->is_dir) {
      int rc = exfat_write_directory(out, child, 0, bitmap_clusters, upcase_clusters, cluster_count);
      if (rc) return rc;
      rc = exfat_write_directories(out, child, 0, bitmap_clusters, upcase_clusters, cluster_count);
      if (rc) return rc;
    }
  }
  (void)root;
  return 0;
}

static void exfat_chain(unsigned char *fat, uint32_t first, uint32_t count) {
  if (!count) return;
  for (uint32_t i = 0; i + 1 < count; i++) put_u32le(fat + (first + i) * 4u, first + i + 1);
  put_u32le(fat + (first + count - 1u) * 4u, 0xFFFFFFFFu);
}

static void exfat_chain_tree(unsigned char *fat, const exfat_node_t *node) {
  exfat_chain(fat, node->first_cluster, node->cluster_count);
  for (size_t i = 0; i < node->child_count; i++) exfat_chain_tree(fat, node->children[i]);
}

static int exfat_write_file_data(FILE *out, const exfat_node_t *node, volatile int *cancel_requested, mkpfs_progress_callback progress, void *opaque, uint64_t *done, uint64_t total) {
  if (node->is_dir) {
    for (size_t i = 0; i < node->child_count; i++) { int rc = exfat_write_file_data(out, node->children[i], cancel_requested, progress, opaque, done, total); if (rc) return rc; }
    return 0;
  }
  if (!node->size) return 0;
  FILE *in = fopen(node->path, "rb"); unsigned char *buffer = (unsigned char *)malloc(1024 * 1024); if (!in || !buffer) { if (in) fclose(in); free(buffer); return in ? ENOMEM : errno; }
  if (fseeko(out, (off_t)node->first_cluster * 65536, SEEK_SET) != 0) { fclose(in); free(buffer); return EIO; }
  uint64_t remaining = node->size;
  while (remaining) {
    if (cancel_requested && *cancel_requested) { fclose(in); free(buffer); return ECANCELED; }
    size_t want = remaining > 1024 * 1024 ? 1024 * 1024 : (size_t)remaining;
    size_t got = fread(buffer, 1, want, in); if (got != want || write_all(out, buffer, got)) { fclose(in); free(buffer); return EIO; }
    remaining -= got; *done += got;
    if (progress && progress(*done, total, "exfat", node->path, opaque)) { fclose(in); free(buffer); return ECANCELED; }
  }
  int rc = write_zeros(out, (uint64_t)node->cluster_count * 65536u - node->size); fclose(in); free(buffer); return rc;
}

static int exfat_read_title_id(const char *source, char *out, size_t out_size) {
  char path[PATH_MAX]; char data[4096]; FILE *f; size_t n; const char *key, *value, *end;
  if (snprintf(path, sizeof(path), "%s/sce_sys/param.json", source) >= (int)sizeof(path)) return ENAMETOOLONG;
  f = fopen(path, "rb"); if (!f) return errno;
  n = fread(data, 1, sizeof(data) - 1, f); fclose(f); data[n] = 0;
  key = strstr(data, "\"titleId\""); if (!key) return ENOENT;
  value = strchr(key + 10, ':'); if (!value) return EINVAL; while (*++value == ' ' || *value == '\t' || *value == '\"') {}
  end = value; while (*end && *end != '\"' && *end != ',' && *end != '}' && (size_t)(end - value) < out_size - 1) end++;
  if (end == value || (size_t)(end - value) >= out_size) return EINVAL;
  memcpy(out, value, (size_t)(end - value)); out[end - value] = 0;
  for (size_t i = 0; out[i]; i++) if (!(isalnum((unsigned char)out[i]) || out[i] == '_')) return EINVAL;
  return 0;
}

static int exfat_write_folder(const char *source, const char *output, volatile int *cancel_requested, mkpfs_progress_callback progress, void *opaque, uint64_t *image_size) {
  int error = 0; exfat_node_t *root = exfat_scan_tree(source, "/", 1, &error); FILE *out = NULL; unsigned char *fat = NULL; unsigned char boot[12 * 512]; uint32_t bitmap_clusters = 1, upcase_clusters, content_clusters, cluster_count, next_cluster, fat_entries, fat_sectors, heap_offset, volume_length; uint64_t total_bytes = 0, done = 0; char temp[PATH_MAX];
  if (!root) return error ? error : EIO;
  upcase_clusters = (MKPFS_EXFAT_UPCASE_SIZE + 65535u) / 65536u;
  content_clusters = exfat_tree_clusters(root, 1) + upcase_clusters;
  while (bitmap_clusters != (uint32_t)(((bitmap_clusters + content_clusters + 7u) / 8u + 65535u) / 65536u)) bitmap_clusters = (uint32_t)(((bitmap_clusters + content_clusters + 7u) / 8u + 65535u) / 65536u);
  cluster_count = bitmap_clusters + content_clusters;
  next_cluster = 2 + bitmap_clusters + upcase_clusters;
  exfat_assign_clusters(root, 1, &next_cluster);
  fat_entries = cluster_count + 2; fat_sectors = ((fat_entries * 4u + 511u) / 512u + 127u) / 128u * 128u; heap_offset = ((128u + fat_sectors + 127u) / 128u) * 128u; volume_length = heap_offset + cluster_count * 128u;
  for (size_t i = 0; i < root->child_count; i++) total_bytes += root->children[i]->size;
  if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", output, (long)getpid()) >= (int)sizeof(temp)) { exfat_free_tree(root); return ENAMETOOLONG; }
  out = fopen(temp, "wb+"); if (!out) { exfat_free_tree(root); return errno; }
  memset(boot, 0, sizeof(boot)); boot[0] = 0xEB; boot[1] = 0x76; boot[2] = 0x90; memcpy(boot + 3, "EXFAT   ", 8); put_u64le(boot + 72, volume_length); put_u32le(boot + 80, 128); put_u32le(boot + 84, fat_sectors); put_u32le(boot + 88, heap_offset); put_u32le(boot + 92, cluster_count); put_u32le(boot + 96, root->first_cluster); put_u32le(boot + 100, 0x4D6B5046u); put_u16le(boot + 104, 0x0100); boot[108] = 9; boot[109] = 7; boot[110] = 1; boot[111] = 0x80; boot[112] = 0xFF; put_u16le(boot + 510, 0xAA55); for (int s = 1; s <= 8; s++) put_u32le(boot + s * 512 + 508, 0xAA550000u); uint32_t checksum = 0; for (size_t i = 0; i < 11 * 512; i++) if (i != 106 && i != 107 && i != 112) checksum = ((checksum << 31) | (checksum >> 1)) + boot[i]; for (int i = 11 * 512; i < 12 * 512; i += 4) put_u32le(boot + i, checksum);
  if (write_all(out, boot, sizeof(boot)) || write_all(out, boot, sizeof(boot)) || write_zeros(out, (128u - 24u) * 512u)) { error = EIO; goto exfat_failed; }
  fat = (unsigned char *)calloc(1, (size_t)fat_sectors * 512u); if (!fat) { error = ENOMEM; goto exfat_failed; } put_u32le(fat, 0xFFFFFFF8u); put_u32le(fat + 4, 0xFFFFFFFFu); exfat_chain(fat, 2, bitmap_clusters); exfat_chain(fat, 2 + bitmap_clusters, upcase_clusters); exfat_chain_tree(fat, root); if (write_all(out, fat, (size_t)fat_sectors * 512u) || write_zeros(out, (uint64_t)(heap_offset - 128u - fat_sectors) * 512u)) { error = EIO; goto exfat_failed; }
  unsigned char *bitmap = (unsigned char *)calloc(1, (size_t)bitmap_clusters * 65536u); if (!bitmap) { error = ENOMEM; goto exfat_failed; } memset(bitmap, 0xFF, (size_t)((cluster_count + 7u) / 8u)); if (write_all(out, bitmap, (size_t)bitmap_clusters * 65536u) || write_all(out, mkpfs_exfat_upcase, MKPFS_EXFAT_UPCASE_SIZE) || write_zeros(out, (uint64_t)upcase_clusters * 65536u - MKPFS_EXFAT_UPCASE_SIZE)) { free(bitmap); error = EIO; goto exfat_failed; } free(bitmap);
  if (exfat_write_directory(out, root, 1, bitmap_clusters, upcase_clusters, cluster_count) || exfat_write_directories(out, root, 1, bitmap_clusters, upcase_clusters, cluster_count)) { error = EIO; goto exfat_failed; }
  if (exfat_write_file_data(out, root, cancel_requested, progress, opaque, &done, total_bytes)) { error = ECANCELED; goto exfat_failed; }
  if (fflush(out) != 0 || fclose(out) != 0 || rename(temp, output) != 0) { error = errno ? errno : EIO; unlink(temp); goto exfat_done; }
  *image_size = (uint64_t)volume_length * 512u; error = 0; goto exfat_done;
exfat_failed: if (out) fclose(out); unlink(temp);
exfat_done: free(fat); exfat_free_tree(root); return error;
}

int mkpfs_build_exfat_folder(const char *source, const char *output_path, volatile int *cancel_requested, mkpfs_progress_callback progress, void *opaque) {
  uint64_t image_size = 0;
  return exfat_write_folder(source, output_path, cancel_requested, progress, opaque, &image_size);
}

int mkpfs_convert_folder_progress(const char *source, const char *destination, const char *output_name, const mkpfs_native_options_t *options, volatile int *cancel_requested, mkpfs_progress_callback progress, void *opaque) {
  mkpfs_scan_result_t scan;
  struct stat st;
  char normalized[PATH_MAX], output[PATH_MAX], exfat_temp[PATH_MAX], inner_name[NAME_MAX + 16];
  int level = options && options->compression_level <= 9 ? (int)options->compression_level : 7;
  uint64_t exfat_size = 0;
  int rc;
  if (!source || !destination || !output_name || !*output_name || strchr(output_name, '/') || strchr(output_name, '\\')) return EINVAL;
  if (mkpfs_normalize_path(source, normalized, sizeof(normalized)) != 0) return EINVAL;
  if (mkpfs_scan_folder(normalized, &scan) != 0) return errno ? errno : EIO;
  if (stat(destination, &st) != 0) return errno;
  if (!S_ISDIR(st.st_mode)) return ENOTDIR;
  if (snprintf(output, sizeof(output), "%s/%s", destination, output_name) >= (int)sizeof(output)) return ENAMETOOLONG;
  if (snprintf(exfat_temp, sizeof(exfat_temp), "%s.exfat.tmp.%ld", output, (long)getpid()) >= (int)sizeof(exfat_temp)) return ENAMETOOLONG;
  char title_id[NAME_MAX];
  if (exfat_read_title_id(normalized, title_id, sizeof(title_id)) == 0) {
    if (snprintf(inner_name, sizeof(inner_name), "%s.exfat", title_id) >= (int)sizeof(inner_name)) return ENAMETOOLONG;
  } else if (snprintf(inner_name, sizeof(inner_name), "%s.exfat", output_name) >= (int)sizeof(inner_name)) return ENAMETOOLONG;
  rc = exfat_write_folder(normalized, exfat_temp, cancel_requested, progress, opaque, &exfat_size);
  if (rc) { unlink(exfat_temp); return rc; }
  rc = mkpfs_wrap_exfat_file(exfat_temp, output, inner_name, level, cancel_requested, progress, opaque);
  unlink(exfat_temp);
  return rc;
}

int mkpfs_convert_folder(const char *source, const char *destination, const char *output_name, const mkpfs_native_options_t *options, volatile int *cancel_requested) {
  return mkpfs_convert_folder_progress(source, destination, output_name, options, cancel_requested, NULL, NULL);
}
