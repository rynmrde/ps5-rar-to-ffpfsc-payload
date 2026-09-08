#define _GNU_SOURCE
#include "mkpfs_native.h"
#include "exfat_upcase.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define PFSC_MAX_OFFSETS_BYTES (64u * 1024u * 1024u)
#define PFSC_PROGRESS_INTERVAL (4u * 1024u * 1024u)
#define PFSC_OFFSET_WRITE_ENTRIES 8192u
#define PFSC_VERIFY_OFFSET_WINDOW 4096u
#define EXFAT_MAX_TREE_NODES 262144u
#define EXFAT_MAX_TREE_DEPTH 128u
/* A checkpoint is made only after complete file records are durable.  This
 * bounds rework after a payload restart without forcing an fsync per file. */
#define EXFAT_CHECKPOINT_BYTES (128u * 1024u * 1024u)
#define EXFAT_CHECKPOINT_FILES 4096u

static int cancellation_requested(const atomic_int *cancel_requested) {
  return cancel_requested && atomic_load_explicit(cancel_requested,
                                                   memory_order_acquire);
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static int add_u64_checked(uint64_t left, uint64_t right, uint64_t *out) {
  if (left > UINT64_MAX - right) return EOVERFLOW;
  *out = left + right;
  return 0;
}

static int multiply_u64_checked(uint64_t left, uint64_t right, uint64_t *out) {
  if (left && right > UINT64_MAX / left) return EOVERFLOW;
  *out = left * right;
  return 0;
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
    if (S_ISREG(st.st_mode)) {
      if (result->file_count == UINT64_MAX ||
          (uint64_t)st.st_size > UINT64_MAX - result->total_bytes) {
        closedir(dir);
        return EOVERFLOW;
      }
      result->file_count++;
      result->total_bytes += (uint64_t)st.st_size;
    }
    else if (S_ISDIR(st.st_mode)) {
      if (result->directory_count == UINT64_MAX) {
        closedir(dir);
        return EOVERFLOW;
      }
      result->directory_count++;
      rc = scan_dir(child, result);
      if (rc) { closedir(dir); return rc; }
    }
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

static int
estimate_workspace_from_exfat(uint64_t exfat_bytes, uint64_t *bytes_out) {
  uint64_t block_count;
  uint64_t offsets_bytes;
  uint64_t pfsc_data_offset;
  uint64_t pfs_bytes;
  uint64_t peak_bytes;
  int rc;

  if(!exfat_bytes || exfat_bytes % PFSC_BLOCK_SIZE || !bytes_out) return EINVAL;
  block_count = exfat_bytes / PFSC_BLOCK_SIZE;
  if(block_count > (UINT64_MAX / sizeof(uint64_t)) - 1u) return EOVERFLOW;
  offsets_bytes = (block_count + 1u) * sizeof(uint64_t);
  if(offsets_bytes > PFSC_MAX_OFFSETS_BYTES) return EFBIG;
  if(offsets_bytes > UINT64_MAX - PFSC_OFFSETS_OFFSET -
                     (PFSC_BLOCK_SIZE - 1u)) return EOVERFLOW;
  pfsc_data_offset = align_up_u64(PFSC_OFFSETS_OFFSET + offsets_bytes,
                                  PFSC_BLOCK_SIZE);
  /* A PFSC block can be stored uncompressed, so this is its maximum size. */
  if((rc = add_u64_checked(pfsc_data_offset, exfat_bytes, &pfs_bytes)) ||
     (rc = add_u64_checked(pfs_bytes, 6u * PFSC_BLOCK_SIZE, &pfs_bytes)) ||
     (rc = add_u64_checked(exfat_bytes, pfs_bytes, &peak_bytes))) return rc;
  *bytes_out = peak_bytes;
  return 0;
}

int
mkpfs_estimate_workspace_from_exfat(uint64_t exfat_bytes, uint64_t *bytes_out) {
  return estimate_workspace_from_exfat(exfat_bytes, bytes_out);
}

int mkpfs_estimate_conversion_workspace(const mkpfs_scan_result_t *scan,
                                        uint64_t *bytes_out) {
  uint64_t padded_file_bytes;
  uint64_t tree_nodes;
  uint64_t directory_entry_bytes;
  uint64_t minimum_directory_bytes;
  uint64_t directory_bytes;
  uint64_t content_bytes;
  uint64_t content_clusters;
  uint64_t bitmap_clusters = 1;
  uint64_t cluster_count;
  uint64_t fat_sectors;
  uint64_t heap_offset_sectors;
  uint64_t exfat_bytes;
  int rc;

  if (!scan || !bytes_out) return EINVAL;

  /* Every regular source file is represented by an integral 64 KiB exFAT
   * allocation.  Use an upper bound of one extra cluster per file rather than
   * walking the source a second time. */
  if (scan->file_count > UINT64_MAX / (PFSC_BLOCK_SIZE - 1u) ||
      scan->total_bytes > UINT64_MAX -
                          scan->file_count * (PFSC_BLOCK_SIZE - 1u)) {
    return EOVERFLOW;
  }
  padded_file_bytes = scan->total_bytes +
                      scan->file_count * (PFSC_BLOCK_SIZE - 1u);
  padded_file_bytes = (padded_file_bytes / PFSC_BLOCK_SIZE) * PFSC_BLOCK_SIZE;

  /* A directory entry set has at least two entries. The maximum 255-byte
   * source name uses 17 name entries, so 19 32-byte entries per child is a
   * conservative deterministic bound. Every directory, including an empty
   * one, allocates at least one 64 KiB cluster. */
  if ((rc = add_u64_checked(scan->file_count, scan->directory_count,
                            &tree_nodes)) ||
      (rc = multiply_u64_checked(tree_nodes, 19u * 32u,
                                 &directory_entry_bytes)) ||
      (rc = add_u64_checked(scan->directory_count, 1u, &tree_nodes)) ||
      (rc = multiply_u64_checked(tree_nodes, PFSC_BLOCK_SIZE,
                                 &minimum_directory_bytes)) ||
      (rc = add_u64_checked(minimum_directory_bytes, 96u,
                            &directory_bytes)) ||
      (rc = add_u64_checked(directory_bytes, directory_entry_bytes,
                            &directory_bytes))) return rc;

  if ((rc = add_u64_checked(padded_file_bytes, directory_bytes,
                            &content_bytes)) ||
      (rc = add_u64_checked(content_bytes, PFSC_BLOCK_SIZE, &content_bytes))) {
    return rc;
  }
  content_clusters = content_bytes / PFSC_BLOCK_SIZE;
  if (content_clusters > UINT32_MAX - 65536u) return EFBIG;

  /* Match exfat_write_folder's bitmap convergence and its fixed 128-sector
   * boot region plus FAT/heap alignment. */
  for (;;) {
    uint64_t required_bitmap_bytes;
    uint64_t next_bitmap_clusters;
    if ((rc = add_u64_checked(bitmap_clusters, content_clusters,
                              &required_bitmap_bytes))) return rc;
    if (required_bitmap_bytes > UINT64_MAX - 7u) return EOVERFLOW;
    required_bitmap_bytes = (required_bitmap_bytes + 7u) / 8u;
    if (required_bitmap_bytes > UINT64_MAX - (PFSC_BLOCK_SIZE - 1u)) {
      return EOVERFLOW;
    }
    next_bitmap_clusters = (required_bitmap_bytes + PFSC_BLOCK_SIZE - 1u) /
                           PFSC_BLOCK_SIZE;
    if (next_bitmap_clusters == bitmap_clusters) break;
    bitmap_clusters = next_bitmap_clusters;
  }
  if ((rc = add_u64_checked(bitmap_clusters, content_clusters,
                            &cluster_count))) return rc;
  if (cluster_count > UINT32_MAX - 2u) return EFBIG;
  if ((rc = add_u64_checked(cluster_count, 2u, &fat_sectors))) return rc;
  if ((rc = multiply_u64_checked(fat_sectors, 4u, &fat_sectors))) return rc;
  if (fat_sectors > UINT64_MAX - 511u) return EOVERFLOW;
  fat_sectors = (fat_sectors + 511u) / 512u;
  if (fat_sectors > UINT64_MAX - 127u) return EOVERFLOW;
  fat_sectors = ((fat_sectors + 127u) / 128u) * 128u;
  if ((rc = add_u64_checked(128u, fat_sectors, &heap_offset_sectors))) return rc;
  if (heap_offset_sectors > UINT64_MAX - 127u) return EOVERFLOW;
  heap_offset_sectors = ((heap_offset_sectors + 127u) / 128u) * 128u;
  if ((rc = multiply_u64_checked(cluster_count, 128u, &exfat_bytes)) ||
      (rc = add_u64_checked(exfat_bytes, heap_offset_sectors, &exfat_bytes)) ||
      (rc = multiply_u64_checked(exfat_bytes, 512u, &exfat_bytes))) return rc;

  return estimate_workspace_from_exfat(exfat_bytes, bytes_out);
}

static int write_zeros(FILE *out, uint64_t count) {
  /* Small files commonly leave almost an entire 64 KiB cluster as padding.
     A 1 MiB bounded buffer avoids thousands of tiny fwrite calls without
     changing the serialized bytes or memory behavior for file contents. */
  static const unsigned char zeros[1024 * 1024] = {0};
  while (count) { size_t n = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count; if (write_all(out, zeros, n)) return EIO; count -= n; }
  return 0;
}

static int write_buffer_flush(FILE *out, unsigned char *buffer, size_t *used) {
  if (*used && write_all(out, buffer, *used)) return EIO;
  *used = 0;
  return 0;
}

static int cleanup_pack(FILE *in, FILE *out, const char *temp_path, int error) {
  if (in) (void)fclose(in);
  if (out) (void)fclose(out);
  if (temp_path) unlink(temp_path);
  return error;
}

static int open_secure_temp_file(const char *output_path, const char *label,
                                 char *temp_path, size_t temp_path_size,
                                 FILE **out) {
  int fd;
  int n;
  struct stat st;

  if (!output_path || !label || !temp_path || !out) return EINVAL;
  if (!lstat(output_path, &st)) return EEXIST;
  if (errno != ENOENT) return errno;
  n = snprintf(temp_path, temp_path_size, "%s.%s.XXXXXX", output_path, label);
  if (n < 0 || (size_t)n >= temp_path_size) return ENAMETOOLONG;
  fd = mkstemp(temp_path);
  if (fd < 0) return errno;
  *out = fdopen(fd, "wb+");
  if (!*out) {
    int error = errno;
    close(fd);
    unlink(temp_path);
    return error;
  }
  return 0;
}

typedef struct pfsc_slot {
  pthread_mutex_t *lock;
  pthread_cond_t *ready;
  int pending;
  int claimed;
  int done;
  int stop;
  int error;
  uint64_t index;
  size_t compressed_size;
  unsigned char *raw;
  unsigned char *compressed;
} pfsc_slot_t;

typedef struct pfsc_pool {
  int count;
  int stopping;
  pthread_mutex_t lock;
  pthread_cond_t work;
  pthread_cond_t result;
  pfsc_slot_t *slots;
  int compression_level;
  const atomic_int *cancel_requested;
} pfsc_pool_t;

static void *pfsc_worker(void *opaque) {
  pfsc_pool_t *pool = (pfsc_pool_t *)opaque;
  for (;;) {
    pfsc_slot_t *slot = NULL;
    pthread_mutex_lock(&pool->lock);
    for (;;) {
      if (pool->stopping) { pthread_mutex_unlock(&pool->lock); return NULL; }
      for (int i = 0; i < pool->count; i++) if (pool->slots[i].pending && !pool->slots[i].claimed && !pool->slots[i].done) { slot = &pool->slots[i]; pool->slots[i].claimed = 1; break; }
      if (slot) break;
      pthread_cond_wait(&pool->work, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
    if (cancellation_requested(pool->cancel_requested)) {
      pthread_mutex_lock(&pool->lock);     slot->error = ECANCELED; slot->claimed = 0; slot->done = 1; pthread_cond_broadcast(&pool->result); pthread_mutex_unlock(&pool->lock); continue;
    }
    uLongf size = compressBound(PFSC_BLOCK_SIZE);
    int zrc = compress2(slot->compressed, &size, slot->raw, PFSC_BLOCK_SIZE, pool->compression_level);
    pthread_mutex_lock(&pool->lock);
    slot->compressed_size = (size_t)size;
    slot->error = zrc == Z_OK ? 0 : EIO;
    slot->claimed = 0;
    slot->done = 1;
    pthread_cond_broadcast(&pool->result);
    pthread_mutex_unlock(&pool->lock);
  }
}

static int pfsc_auto_workers(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  if (n > 8) n = 8;
  return (int)n;
}

static int pack_pfsc_into_stream(const char *input_path, FILE *out, uint64_t base_offset,
                                 int compression_level, unsigned int requested_workers,
                                 const atomic_int *cancel_requested,
                                 mkpfs_progress_callback progress, void *opaque,
                                 uint64_t *stored_size_out,
                                 mkpfs_resume_state_t *resume,
                                 mkpfs_resume_checkpoint_callback checkpoint,
                                 void *checkpoint_opaque) {
  struct stat st; FILE *in = NULL;
  uint64_t *offsets = NULL; uint64_t logical_size, block_count, offsets_bytes, data_offset, stored_pos, last_reported = 0;
  uint64_t next_dispatch = 0, next_write = 0;
  uint64_t resumed_blocks = 0;
  uint64_t checkpoint_from = 0;
  int rc = 0;
  pfsc_pool_t pool = {0}; pthread_t *threads = NULL; int threads_started = 0;
  int pool_initialized = 0;
  unsigned char *write_buffer = NULL;
  size_t write_buffer_used = 0;
  if (!input_path || !out || stat(input_path, &st) != 0) return errno ? errno : EINVAL;
  if (!S_ISREG(st.st_mode) || compression_level < 0 || compression_level > 9) return EINVAL;
  logical_size = align_up_u64((uint64_t)st.st_size, PFSC_BLOCK_SIZE); if (!logical_size) logical_size = PFSC_BLOCK_SIZE;
  block_count = logical_size / PFSC_BLOCK_SIZE;
  if (block_count > (SIZE_MAX / sizeof(*offsets)) - 1) return EOVERFLOW;
  offsets_bytes = (block_count + 1) * sizeof(*offsets);
  if (offsets_bytes > PFSC_MAX_OFFSETS_BYTES) return EFBIG;
  data_offset = align_up_u64(PFSC_OFFSETS_OFFSET + offsets_bytes, PFSC_INITIAL_DATA_OFFSET);
  in = fopen(input_path, "rb"); if (!in) return errno;
  offsets = (uint64_t *)calloc((size_t)(block_count + 1), sizeof(*offsets));
  if (!offsets) { fclose(in); return ENOMEM; }
  write_buffer = (unsigned char *)malloc(PFSC_BLOCK_SIZE);
  if (!write_buffer) { rc = ENOMEM; goto failed; }
  unsigned char header[PFSC_HEADER_SIZE] = {0};
  put_u32le(header + 0x00, PFSC_MAGIC); put_u32le(header + 0x04, PFSC_UNK4); put_u32le(header + 0x08, PFSC_UNK8); put_u32le(header + 0x0c, PFSC_BLOCK_SIZE); put_u64le(header + 0x10, PFSC_BLOCK_SIZE); put_u64le(header + 0x18, PFSC_OFFSETS_OFFSET); put_u64le(header + 0x20, data_offset); put_u64le(header + 0x28, logical_size);
  if(resume && resume->phase == MKPFS_RESUME_PACK && resume->pack_next_block) {
    resumed_blocks = resume->pack_next_block;
    if(resumed_blocks > block_count || resume->pack_stored_size < data_offset ||
       fseeko(out, (off_t)(base_offset + PFSC_OFFSETS_OFFSET), SEEK_SET) != 0 ||
       read_all(out, offsets, (size_t)(resumed_blocks + 1u) * sizeof(*offsets)) ||
       offsets[0] != data_offset || offsets[resumed_blocks] != resume->pack_stored_size ||
       fseeko(in, (off_t)(resumed_blocks * PFSC_BLOCK_SIZE), SEEK_SET) != 0 ||
       ftruncate(fileno(out), (off_t)(base_offset + resume->pack_stored_size)) != 0) {
      rc = EINVAL;
      goto failed;
    }
    stored_pos = resume->pack_stored_size;
    next_dispatch = resumed_blocks;
    next_write = resumed_blocks;
    checkpoint_from = resumed_blocks;
    last_reported = resumed_blocks * PFSC_BLOCK_SIZE;
    if(last_reported > (uint64_t)st.st_size) last_reported = (uint64_t)st.st_size;
  } else {
    if (fseeko(out, (off_t)base_offset, SEEK_SET) != 0 ||
        (rc = write_all(out, header, sizeof(header))) ||
        (rc = write_zeros(out, data_offset - sizeof(header)))) { if (!rc) rc = EIO; goto failed; }
    stored_pos = data_offset;
  }
  if(fseeko(out, (off_t)(base_offset + stored_pos), SEEK_SET) != 0) {
    rc = EIO;
    goto failed;
  }
  pool.count = requested_workers ? (requested_workers > 8 ? 8 : (int)requested_workers) : pfsc_auto_workers();
  if (pool.count < 1) pool.count = 1;
  pool.compression_level = compression_level; pool.cancel_requested = cancel_requested;
  if (pthread_mutex_init(&pool.lock, NULL) || pthread_cond_init(&pool.work, NULL) || pthread_cond_init(&pool.result, NULL)) {
    rc = EAGAIN;
    goto failed;
  }
  pool_initialized = 1;
  pool.slots = (pfsc_slot_t *)calloc((size_t)pool.count, sizeof(*pool.slots)); threads = (pthread_t *)calloc((size_t)pool.count, sizeof(*threads));
  if (!pool.slots || !threads) { rc = ENOMEM; goto failed_pool; }
  for (int i = 0; i < pool.count; i++) {
    pool.slots[i].raw = (unsigned char *)calloc(1, PFSC_BLOCK_SIZE);
    pool.slots[i].compressed = (unsigned char *)malloc(compressBound(PFSC_BLOCK_SIZE));
    if (!pool.slots[i].raw || !pool.slots[i].compressed) { rc = ENOMEM; goto failed_pool; }
  }
  /* A single requested worker must remain deterministic, but creating a
     separate compression thread for every 64 KiB PFSC block adds a mutex and
     condition-variable round trip.  Compress inline in that safe, bounded
     case; multi-worker requests retain the established parallel queue. */
  if (pool.count > 1) {
    for (int i = 0; i < pool.count; i++) {
      if (pthread_create(&threads[i], NULL, pfsc_worker, &pool) != 0) {
        rc = EAGAIN;
        goto failed_pool;
      }
      threads_started++;
    }
  }
  if (progress && progress(last_reported, (uint64_t)st.st_size, "compress", input_path, opaque)) {
    rc = ECANCELED;
    goto failed_pool;
  }
  while (next_write < block_count) {
    while (next_dispatch < block_count && next_dispatch - next_write < (uint64_t)pool.count) {
      pfsc_slot_t *slot = &pool.slots[next_dispatch % (uint64_t)pool.count];
      memset(slot->raw, 0, PFSC_BLOCK_SIZE);
      size_t got = fread(slot->raw, 1, PFSC_BLOCK_SIZE, in);
      if (ferror(in) || (got != PFSC_BLOCK_SIZE && next_dispatch + 1 < block_count)) {
        rc = EIO;
        goto failed_pool;
      }
      if (pool.count == 1) {
        uLongf size = compressBound(PFSC_BLOCK_SIZE);
        if (cancellation_requested(cancel_requested)) { rc = ECANCELED; goto failed_pool; }
        slot->index = next_dispatch;
        slot->compressed_size = 0;
        slot->error = compress2(slot->compressed, &size, slot->raw,
                                PFSC_BLOCK_SIZE, pool.compression_level) == Z_OK ? 0 : EIO;
        slot->compressed_size = (size_t)size;
        slot->done = 1;
        slot->pending = 1;
        next_dispatch++;
      } else {
        pthread_mutex_lock(&pool.lock);
        slot->index = next_dispatch; slot->compressed_size = 0; slot->error = 0; slot->claimed = 0; slot->done = 0; slot->pending = 1; next_dispatch++;
        pthread_cond_signal(&pool.work); pthread_mutex_unlock(&pool.lock);
      }
    }
    if (cancellation_requested(cancel_requested)) { rc = ECANCELED; goto failed_pool; }
    pfsc_slot_t *slot = &pool.slots[next_write % (uint64_t)pool.count];
    if (pool.count == 1) {
      if (cancellation_requested(cancel_requested)) { rc = ECANCELED; goto failed_pool; }
      rc = slot->error;
    } else {
      pthread_mutex_lock(&pool.lock);
      while (!slot->done && !cancellation_requested(cancel_requested)) pthread_cond_wait(&pool.result, &pool.lock);
      if (cancellation_requested(cancel_requested)) { pthread_mutex_unlock(&pool.lock); rc = ECANCELED; goto failed_pool; }
      rc = slot->error;
      pthread_mutex_unlock(&pool.lock);
    }
    if (rc) goto failed_pool;
    offsets[next_write] = stored_pos;
    if (slot->compressed_size < PFSC_BLOCK_SIZE) {
      if (slot->compressed_size > PFSC_BLOCK_SIZE - write_buffer_used &&
          (rc = write_buffer_flush(out, write_buffer, &write_buffer_used))) goto failed_pool;
      memcpy(write_buffer + write_buffer_used, slot->compressed, slot->compressed_size);
      write_buffer_used += slot->compressed_size;
      stored_pos += slot->compressed_size;
    } else {
      if ((rc = write_buffer_flush(out, write_buffer, &write_buffer_used)) ||
          (rc = write_all(out, slot->raw, PFSC_BLOCK_SIZE))) goto failed_pool;
      stored_pos += PFSC_BLOCK_SIZE;
    }
    if (rc) goto failed_pool;
    if (pool.count == 1) {
      slot->pending = 0;
      slot->done = 0;
    } else {
      pthread_mutex_lock(&pool.lock); slot->pending = 0; slot->done = 0; pthread_mutex_unlock(&pool.lock);
    }
    next_write++;
    /* The checkpoint offset table needs both the first block offset and the
     * end offset after the last durable block in its window. */
    offsets[next_write] = stored_pos;
    if(resume && checkpoint &&
       (next_write == block_count || next_write - checkpoint_from >= 128u)) {
      unsigned char checkpoint_offsets[(128u + 1u) * 8u];
      size_t checkpoint_size = 0;

      if((rc = write_buffer_flush(out, write_buffer, &write_buffer_used)) ||
         fseeko(out, (off_t)(base_offset + PFSC_OFFSETS_OFFSET +
                             checkpoint_from * 8u), SEEK_SET) != 0) {
        if(!rc) rc = EIO;
        goto failed_pool;
      }
      for(uint64_t i = checkpoint_from; i <= next_write; i++) {
        put_u64le(checkpoint_offsets + checkpoint_size, offsets[i]);
        checkpoint_size += 8u;
      }
      if(write_all(out, checkpoint_offsets, checkpoint_size) || fflush(out) != 0 ||
         fsync(fileno(out)) != 0) {
        rc = EIO;
        goto failed_pool;
      }
      resume->phase = MKPFS_RESUME_PACK;
      resume->pack_next_block = next_write;
      resume->pack_stored_size = stored_pos;
      if(checkpoint(resume, checkpoint_opaque)) {
        rc = EIO;
        goto failed_pool;
      }
      if(fseeko(out, (off_t)(base_offset + stored_pos), SEEK_SET) != 0) {
        rc = EIO;
        goto failed_pool;
      }
      checkpoint_from = next_write;
    }
    {
      uint64_t completed = next_write * PFSC_BLOCK_SIZE > (uint64_t)st.st_size ?
                           (uint64_t)st.st_size : next_write * PFSC_BLOCK_SIZE;
      if (progress && (completed == (uint64_t)st.st_size ||
                       completed - last_reported >= PFSC_PROGRESS_INTERVAL)) {
        if (progress(completed, (uint64_t)st.st_size, "compress", input_path, opaque)) {
          rc = ECANCELED;
          goto failed_pool;
        }
        last_reported = completed;
      }
    }
  }
  if ((rc = write_buffer_flush(out, write_buffer, &write_buffer_used))) goto failed_pool;
  pthread_mutex_lock(&pool.lock); pool.stopping = 1; pthread_cond_broadcast(&pool.work); pthread_mutex_unlock(&pool.lock);
  for (int i = 0; i < threads_started; i++) pthread_join(threads[i], NULL);
  threads_started = 0;
  offsets[block_count] = stored_pos;
  if (fseeko(out, (off_t)(base_offset + PFSC_OFFSETS_OFFSET), SEEK_SET) != 0) { rc = EIO; goto failed_pool; }
  {
    unsigned char offset_buffer[PFSC_OFFSET_WRITE_ENTRIES * 8u];
    size_t offset_used = 0;
    for (uint64_t i = 0; i <= block_count; i++) {
      if ((i % PFSC_OFFSET_WRITE_ENTRIES) == 0 && cancellation_requested(cancel_requested)) {
        rc = ECANCELED;
        goto failed_pool;
      }
      put_u64le(offset_buffer + offset_used, offsets[i]);
      offset_used += 8;
      if (offset_used == sizeof(offset_buffer)) {
        if (write_all(out, offset_buffer, offset_used)) { rc = EIO; goto failed_pool; }
        offset_used = 0;
      }
    }
    if (offset_used && write_all(out, offset_buffer, offset_used)) { rc = EIO; goto failed_pool; }
  }
  if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
    rc = EIO;
    goto failed_pool;
  }
  if(resume && checkpoint) {
    resume->phase = MKPFS_RESUME_VERIFY;
    resume->pack_next_block = block_count;
    resume->pack_stored_size = stored_pos;
    resume->verify_next_block = 0;
    if(checkpoint(resume, checkpoint_opaque)) {
      rc = EIO;
      goto failed_pool;
    }
  }
  if (fclose(in) != 0) { in = NULL; rc = EIO; goto failed_pool; }
  in = NULL;
  if (stored_size_out) *stored_size_out = stored_pos;
  rc = 0; goto done_pool;
failed_pool:
  if (pool_initialized) { pthread_mutex_lock(&pool.lock); pool.stopping = 1; pthread_cond_broadcast(&pool.work); pthread_cond_broadcast(&pool.result); pthread_mutex_unlock(&pool.lock); }
  for (int i = 0; i < threads_started; i++) pthread_join(threads[i], NULL);
  threads_started = 0;
  if (in) fclose(in);
  in = NULL;
  goto done_pool;
failed:
  if (in) fclose(in);
  in = NULL;
done_pool:
  if (pool.slots) for (int i = 0; i < pool.count; i++) { free(pool.slots[i].raw); free(pool.slots[i].compressed); }
  free(pool.slots); free(threads); free(offsets); free(write_buffer);
  if (pool_initialized) { pthread_cond_destroy(&pool.work); pthread_cond_destroy(&pool.result); pthread_mutex_destroy(&pool.lock); }
  return rc;
}

int mkpfs_pack_pfsc_file_ex(const char *input_path, const char *output_path,
                            int compression_level, unsigned int requested_workers,
                            const atomic_int *cancel_requested,
                            mkpfs_progress_callback progress, void *opaque) {
  FILE *out = NULL;
  char temp_path[PATH_MAX];
  int rc;

  if (!output_path) return EINVAL;
  rc = open_secure_temp_file(output_path, "pfsc", temp_path, sizeof(temp_path), &out);
  if (rc) return rc;
  if(!out) {
    unlink(temp_path);
    return EIO;
  }
  rc = pack_pfsc_into_stream(input_path, out, 0, compression_level, requested_workers,
                             cancel_requested, progress, opaque, NULL, NULL, NULL,
                             NULL);
  if (rc) { cleanup_pack(NULL, out, temp_path, rc); return rc; }
  if (fflush(out) != 0 || fsync(fileno(out)) != 0) { cleanup_pack(NULL, out, temp_path, EIO); return EIO; }
  if (fclose(out) != 0) { out = NULL; unlink(temp_path); return EIO; }
  out = NULL;
  if (rename(temp_path, output_path) != 0) {
    rc = errno;
    unlink(temp_path);
    return rc;
  }
  return 0;
}

int mkpfs_pack_pfsc_file(const char *input_path, const char *output_path, int compression_level, const atomic_int *cancel_requested, mkpfs_progress_callback progress, void *opaque) {
  return mkpfs_pack_pfsc_file_ex(input_path, output_path, compression_level, 1, cancel_requested, progress, opaque);
}

static int verify_pfsc_stream(FILE *f, uint64_t base_offset,
                              uint64_t *logical_size_out, uint64_t *block_count_out,
                              const atomic_int *cancel_requested,
                              mkpfs_progress_callback progress, void *opaque,
                              mkpfs_resume_state_t *resume,
                              mkpfs_resume_checkpoint_callback checkpoint,
                              void *checkpoint_opaque) {
  unsigned char header[PFSC_HEADER_SIZE]; uint64_t logical_size, block_count, offsets_offset, data_offset, previous = 0, verified = 0, last_reported = 0; int rc = 0;
  uint64_t start_index = 0;
  if (!f) return EINVAL;
  if (fseeko(f, (off_t)base_offset, SEEK_SET) != 0 || read_all(f, header, sizeof(header))) return EINVAL;
  if (get_u32le(header) != PFSC_MAGIC || get_u32le(header + 4) != PFSC_UNK4 || get_u32le(header + 8) != PFSC_UNK8 || get_u32le(header + 12) != PFSC_BLOCK_SIZE || get_u64le(header + 16) != PFSC_BLOCK_SIZE) return EINVAL;
  offsets_offset = get_u64le(header + 24); data_offset = get_u64le(header + 32); logical_size = get_u64le(header + 40);
  if (offsets_offset != PFSC_OFFSETS_OFFSET || data_offset < PFSC_INITIAL_DATA_OFFSET || logical_size == 0 || logical_size % PFSC_BLOCK_SIZE) return EINVAL;
  block_count = logical_size / PFSC_BLOCK_SIZE;
  if (fseeko(f, 0, SEEK_END) != 0) return EIO;
  off_t file_size = ftello(f); if (file_size < 0 || base_offset > (uint64_t)file_size || (uint64_t)file_size - base_offset < data_offset) return EINVAL;
  if(resume && resume->phase == MKPFS_RESUME_VERIFY) {
    start_index = resume->verify_next_block;
    if(start_index > block_count) return EINVAL;
    verified = start_index * PFSC_BLOCK_SIZE;
    if(verified > logical_size) verified = logical_size;
    last_reported = verified;
    if(start_index) {
      unsigned char prior_offset[8];
      if(fseeko(f, (off_t)(base_offset + offsets_offset + start_index * 8u),
                SEEK_SET) != 0 || read_all(f, prior_offset, sizeof(prior_offset))) {
        return EIO;
      }
      previous = get_u64le(prior_offset);
      if(previous < data_offset || previous > (uint64_t)file_size - base_offset) {
        return EINVAL;
      }
    }
  }
  unsigned char *raw = (unsigned char *)malloc(PFSC_BLOCK_SIZE); unsigned char *stored = (unsigned char *)malloc(compressBound(PFSC_BLOCK_SIZE));
  if (!raw || !stored) { free(raw); free(stored); return ENOMEM; }
  if (progress && progress(verified, logical_size, "verify", "verifying PFSC", opaque)) {
    rc = ECANCELED;
    goto verify_done;
  }
  for (uint64_t index = start_index; index < block_count;) {
    unsigned char offset_window[(PFSC_VERIFY_OFFSET_WINDOW + 1u) * 8u];
    uint64_t count = block_count - index;
    uint64_t payload_position = 0;
    if (count > PFSC_VERIFY_OFFSET_WINDOW) count = PFSC_VERIFY_OFFSET_WINDOW;
    if (cancellation_requested(cancel_requested)) { rc = ECANCELED; goto verify_done; }
    if (fseeko(f, (off_t)(base_offset + offsets_offset + index * 8u), SEEK_SET) != 0 ||
        read_all(f, offset_window, (size_t)(count + 1u) * 8u)) { rc = EIO; goto verify_done; }
    for (uint64_t offset_index = 0; offset_index < count; offset_index++) {
      uint64_t start = get_u64le(offset_window + offset_index * 8u);
      uint64_t end = get_u64le(offset_window + (offset_index + 1u) * 8u);
      if (index + offset_index == 0 && start != data_offset) { rc = EINVAL; goto verify_done; }
      if (start < previous || end < start || end > (uint64_t)file_size - base_offset ||
          end - start > PFSC_BLOCK_SIZE) { rc = EINVAL; goto verify_done; }
      previous = end;
    }
    payload_position = get_u64le(offset_window);
    if (fseeko(f, (off_t)(base_offset + payload_position), SEEK_SET) != 0) { rc = EIO; goto verify_done; }
    for (uint64_t offset_index = 0; offset_index < count; offset_index++) {
      uint64_t start = get_u64le(offset_window + offset_index * 8u);
      uint64_t end = get_u64le(offset_window + (offset_index + 1u) * 8u);
      uint64_t stored_size = end - start;
      if (start != payload_position && fseeko(f, (off_t)(base_offset + start), SEEK_SET) != 0) { rc = EIO; goto verify_done; }
      if (read_all(f, stored, (size_t)stored_size)) { rc = EIO; goto verify_done; }
      payload_position = end;
      if (stored_size == PFSC_BLOCK_SIZE) memcpy(raw, stored, PFSC_BLOCK_SIZE);
      else {
        uLongf raw_size = PFSC_BLOCK_SIZE;
        if (uncompress(raw, &raw_size, stored, (uLong)stored_size) != Z_OK || raw_size != PFSC_BLOCK_SIZE) {
          rc = EINVAL;
          goto verify_done;
        }
      }
      verified += PFSC_BLOCK_SIZE;
    }
    index += count;
    if(resume && checkpoint) {
      resume->phase = MKPFS_RESUME_VERIFY;
      resume->verify_next_block = index;
      if(checkpoint(resume, checkpoint_opaque)) {
        rc = EIO;
        goto verify_done;
      }
    }
    if (progress && (verified >= logical_size || verified - last_reported >= PFSC_PROGRESS_INTERVAL) &&
        progress(verified > logical_size ? logical_size : verified, logical_size,
                 "verify", "verifying PFSC", opaque)) {
      rc = ECANCELED;
      goto verify_done;
    }
    if (progress && verified - last_reported >= PFSC_PROGRESS_INTERVAL) last_reported = verified;
  }
  if(!rc && resume && checkpoint) {
    resume->phase = MKPFS_RESUME_PUBLISH;
    resume->verify_next_block = block_count;
    if(checkpoint(resume, checkpoint_opaque)) rc = EIO;
  }
verify_done: free(raw); free(stored);
  if (!rc) { if (logical_size_out) *logical_size_out = logical_size; if (block_count_out) *block_count_out = block_count; }
  return rc;
}

static int verify_pfsc_file_impl(const char *path, uint64_t *logical_size_out,
                                 uint64_t *block_count_out,
                                 const atomic_int *cancel_requested,
                                 mkpfs_progress_callback progress, void *opaque) {
  FILE *f;
  int rc;
  if (!path) return EINVAL;
  f = fopen(path, "rb");
  if (!f) return errno;
  rc = verify_pfsc_stream(f, 0, logical_size_out, block_count_out,
                          cancel_requested, progress, opaque, NULL, NULL,
                          NULL);
  fclose(f);
  return rc;
}

int mkpfs_verify_pfsc_file(const char *path, uint64_t *logical_size_out, uint64_t *block_count_out) {
  return verify_pfsc_file_impl(path, logical_size_out, block_count_out, NULL, NULL, NULL);
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

int mkpfs_wrap_exfat_file_ex(const char *exfat_path, const char *output_path,
                             const char *inner_name, int compression_level,
                             unsigned int workers,
                             const atomic_int *cancel_requested,
                          mkpfs_progress_callback progress, void *opaque) {
  struct stat st; char temp_path[PATH_MAX]; FILE *out = NULL;
  unsigned char *inode_table = NULL, *root_dir = NULL; uint64_t pfsc_size, raw_size, pfsc_blocks, final_blocks;
  uint32_t hash; int rc;
  if (!exfat_path || !output_path || !inner_name || !*inner_name) return EINVAL;
  if (stat(exfat_path, &st) != 0 || !S_ISREG(st.st_mode)) return errno ? errno : EINVAL;
  rc = open_secure_temp_file(output_path, "pfs", temp_path, sizeof(temp_path), &out);
  if (rc) return rc;
  if (!out) { unlink(temp_path); return EIO; }
  rc = pack_pfsc_into_stream(exfat_path, out, 6u * 65536u, compression_level, workers,
                             cancel_requested, progress, opaque, &pfsc_size, NULL,
                             NULL, NULL);
  if (rc != 0) goto wrap_failed;
  raw_size = 0;
  rc = verify_pfsc_stream(out, 6u * 65536u, &raw_size, NULL,
                          cancel_requested, progress, opaque, NULL, NULL,
                          NULL);
  if (rc != 0) goto wrap_failed;
  if (cancellation_requested(cancel_requested)) { rc = ECANCELED; goto wrap_failed; }
  pfsc_blocks = (pfsc_size + 65535) / 65536;
  if (pfs_hash_path(inner_name, &hash) != 0) { rc = EINVAL; goto wrap_failed; }
  inode_table = (unsigned char *)calloc(4, 0xA8); root_dir = (unsigned char *)calloc(1, 65536);
  if (!inode_table || !root_dir) { rc = ENOMEM; goto wrap_failed; }
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
  if (progress && progress(0, pfsc_size, "publish", "publishing output", opaque)) { rc = ECANCELED; goto wrap_failed; }
  if (fseeko(out, 0, SEEK_SET) != 0 || write_all(out, header, sizeof(header)) || write_all(out, inode_table, 4 * 0xA8)) { rc = EIO; goto wrap_failed; }
  if (fseeko(out, 2 * 65536, SEEK_SET) != 0 || write_all(out, root_dir, 65536) || fseeko(out, 3 * 65536, SEEK_SET) != 0 || write_all(out, fpt, sizeof(fpt)) || fseeko(out, 5 * 65536, SEEK_SET) != 0 || write_all(out, uroot, sizeof(uroot))) { rc = EIO; goto wrap_failed; }
  if (cancellation_requested(cancel_requested) ||
      (progress && progress(pfsc_size, pfsc_size, "publish", "publishing output", opaque))) { rc = ECANCELED; goto wrap_failed; }
  final_blocks = 6 + pfsc_blocks; put_i64le(header + 0x38, (int64_t)final_blocks); if (fseeko(out, 0, SEEK_SET) != 0 || write_all(out, header, sizeof(header)) || fflush(out) != 0 || fsync(fileno(out)) != 0) { rc = EIO; goto wrap_failed; }
  if (fclose(out) != 0) { out = NULL; rc = EIO; goto wrap_failed_no_out; }
  out = NULL; if (rename(temp_path, output_path) != 0) { rc = errno; goto wrap_failed_no_out; }
  free(inode_table); free(root_dir); return 0;
wrap_failed: if (out) fclose(out);
wrap_failed_no_out: unlink(temp_path); free(inode_table); free(root_dir); return rc;
}

int mkpfs_wrap_exfat_file(const char *exfat_path, const char *output_path, const char *inner_name, int compression_level, const atomic_int *cancel_requested, mkpfs_progress_callback progress, void *opaque) {
  return mkpfs_wrap_exfat_file_ex(exfat_path, output_path, inner_name, compression_level, 1, cancel_requested, progress, opaque);
}

static int
open_resume_stage(const char *path, FILE **out) {
  struct stat st;
  int fd;

  if(!path || !out) return EINVAL;
  if(lstat(path, &st) == 0) {
    if(!S_ISREG(st.st_mode) || st.st_nlink != 1) return EINVAL;
    fd = open(path, O_RDWR | O_NOFOLLOW);
  } else {
    if(errno != ENOENT) return errno;
    fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  }
  if(fd < 0) return errno;
  *out = fdopen(fd, "rb+");
  if(!*out) {
    int error = errno;
    close(fd);
    return error;
  }
  return 0;
}

static int
publish_resumable_pfs(FILE *out, const char *output_path, const char *inner_name,
                      uint64_t pfsc_size, uint64_t raw_size,
                      const atomic_int *cancel_requested,
                      mkpfs_progress_callback progress, void *opaque) {
  unsigned char *inode_table = NULL;
  unsigned char *root_dir = NULL;
  unsigned char header[65536] = {0};
  unsigned char fpt[8];
  unsigned char uroot[65536];
  uint64_t pfsc_blocks;
  uint64_t final_blocks;
  uint32_t hash;
  size_t pos = 0;
  int rc = 0;

  if(!out || !output_path || !inner_name) return EINVAL;
  if(cancellation_requested(cancel_requested)) return ECANCELED;
  pfsc_blocks = (pfsc_size + 65535u) / 65536u;
  if(pfs_hash_path(inner_name, &hash) != 0) return EINVAL;
  inode_table = calloc(4, 0xA8);
  root_dir = calloc(1, 65536);
  if(!inode_table || !root_dir) {
    rc = ENOMEM;
    goto done;
  }
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 0, 4, ".");
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 0, 5, "..");
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 1, 2, "flat_path_table");
  pos += pfs_dirent(root_dir + pos, 65536 - pos, 2, 3, "uroot");
  if(!pos) { rc = EINVAL; goto done; }
  put_u32le(fpt, hash);
  put_u32le(fpt + 4, 3);
  memset(uroot, 0, sizeof(uroot));
  pos = 0;
  pos += pfs_dirent(uroot + pos, sizeof(uroot) - pos, 2, 4, ".");
  pos += pfs_dirent(uroot + pos, sizeof(uroot) - pos, 2, 5, "..");
  pos += pfs_dirent(uroot + pos, sizeof(uroot) - pos, 3, 2, inner_name);
  if(!pos) { rc = EINVAL; goto done; }
  pfs_inode(inode_table + 0 * 0xA8, 0x4405, 1, 0x20010, 65536, 65536, 1, 2, 1);
  pfs_inode(inode_table + 1 * 0xA8, 0x8101, 1, 0x20010, 8, 8, 1, 3, 1);
  pfs_inode(inode_table + 2 * 0xA8, 0x4405, 3, 0x10, 65536, 65536, 1, 5, 1);
  pfs_inode(inode_table + 3 * 0xA8, 0x8101, 1, 0x11, pfsc_size, raw_size,
            pfsc_blocks, 6, 1);
  put_i64le(header + 0x00, 2);
  put_i64le(header + 0x08, 20130315);
  header[0x1a] = 1;
  put_u32le(header + 0x1c, 0x0008);
  put_u32le(header + 0x20, 65536);
  put_i64le(header + 0x28, 1);
  put_i64le(header + 0x30, 4);
  put_i64le(header + 0x40, 1);
  put_u32le(header + 0x368, 1);
  if(progress && progress(0, pfsc_size, "publish", "publishing output", opaque)) {
    rc = ECANCELED;
    goto done;
  }
  if(fseeko(out, 0, SEEK_SET) != 0 ||
     write_all(out, header, sizeof(header)) ||
     write_all(out, inode_table, 4 * 0xA8) ||
     fseeko(out, 2 * 65536, SEEK_SET) != 0 ||
     write_all(out, root_dir, 65536) ||
     fseeko(out, 3 * 65536, SEEK_SET) != 0 ||
     write_all(out, fpt, sizeof(fpt)) ||
     fseeko(out, 5 * 65536, SEEK_SET) != 0 ||
     write_all(out, uroot, sizeof(uroot))) {
    rc = EIO;
    goto done;
  }
  if(cancellation_requested(cancel_requested) ||
     (progress && progress(pfsc_size, pfsc_size, "publish", "publishing output", opaque))) {
    rc = ECANCELED;
    goto done;
  }
  final_blocks = 6 + pfsc_blocks;
  put_i64le(header + 0x38, (int64_t)final_blocks);
  if(ftruncate(fileno(out), (off_t)(6u * 65536u + pfsc_size)) != 0 ||
     fseeko(out, 0, SEEK_SET) != 0 || write_all(out, header, sizeof(header)) ||
     fflush(out) != 0 || fsync(fileno(out)) != 0) {
    rc = EIO;
  }
done:
  free(inode_table);
  free(root_dir);
  return rc;
}

typedef struct exfat_node {
  char *name;
  char *path;
  int is_dir;
  dev_t device;
  ino_t inode;
  uint64_t size;
  int64_t mtime_sec;
  long mtime_nsec;
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

static int exfat_name_compare(const char *a, const char *b) {
  const unsigned char *pa = (const unsigned char *)a;
  const unsigned char *pb = (const unsigned char *)b;
  while (*pa && *pb) {
    int ca = tolower(*pa++), cb = tolower(*pb++);
    if (ca != cb) return ca - cb;
  }
  return *pa - *pb;
}

static int exfat_node_cmp(const void *a, const void *b) {
  const exfat_node_t *const *left = a;
  const exfat_node_t *const *right = b;
  return exfat_name_compare((*left)->name, (*right)->name);
}

static int exfat_ignored_name(const char *name) {
  return !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".DS_Store") || !strcmp(name, "Thumbs.db");
}

static int exfat_cluster_offset(uint32_t heap_offset, uint32_t cluster,
                                uint64_t *offset_out) {
  if (cluster < 2 || !offset_out) return EINVAL;
  *offset_out = (uint64_t)heap_offset * 512u +
                (uint64_t)(cluster - 2u) * 65536u;
  return 0;
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

static exfat_node_t *exfat_scan_tree(const char *path, const char *name, int root, int *error_out, size_t *node_count, size_t depth) {
  struct stat st;
  exfat_node_t *node;
  if (!node_count || depth > EXFAT_MAX_TREE_DEPTH) { *error_out = ELOOP; return NULL; }
  if (*node_count >= EXFAT_MAX_TREE_NODES) { *error_out = EFBIG; return NULL; }
  if (lstat(path, &st) != 0) { *error_out = errno; return NULL; }
  if (S_ISLNK(st.st_mode)) { *error_out = ELOOP; return NULL; }
  if (!root && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { *error_out = EINVAL; return NULL; }
  node = (exfat_node_t *)calloc(1, sizeof(*node));
  if (!node) { *error_out = ENOMEM; return NULL; }
  (*node_count)++;
  node->name = strdup(name ? name : ""); node->path = strdup(path);
  node->is_dir = root || S_ISDIR(st.st_mode); node->device = st.st_dev;
  node->inode = st.st_ino; node->size = node->is_dir ? 0 : (uint64_t)st.st_size;
  node->mtime_sec = (int64_t)st.st_mtim.tv_sec;
  node->mtime_nsec = st.st_mtim.tv_nsec;
  if (!node->is_dir && node->size > (uint64_t)(UINT32_MAX - 2u) * 65536u) { exfat_free_tree(node); *error_out = EFBIG; return NULL; }
  if (!node->name || !node->path) { exfat_free_tree(node); *error_out = ENOMEM; return NULL; }
  if (node->is_dir) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if (!dir) { exfat_free_tree(node); *error_out = errno; return NULL; }
    while ((entry = readdir(dir))) {
      if (!exfat_ignored_name(entry->d_name)) {
        char child_path[PATH_MAX];
        exfat_node_t *child;
        if (snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name) >= (int)sizeof(child_path)) {
          *error_out = ENAMETOOLONG; closedir(dir); exfat_free_tree(node); return NULL;
        }
        child = exfat_scan_tree(child_path, entry->d_name, 0, error_out, node_count, depth + 1);
        if (!child || exfat_add_child(node, child)) {
          if (child) exfat_free_tree(child);
          closedir(dir); exfat_free_tree(node); return NULL;
        }
      }
    }
    closedir(dir);
    if(node->child_count > 1) {
      qsort(node->children, node->child_count, sizeof(*node->children),
            exfat_node_cmp);
    }
    for (size_t i = 1; i < node->child_count; i++) {
      if (!strcasecmp(node->children[i - 1]->name, node->children[i]->name)) {
        *error_out = EEXIST; exfat_free_tree(node); return NULL;
      }
    }
  }
  return node;
}

static uint64_t
exfat_hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;

  for(size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t
exfat_hash_u64(uint64_t hash, uint64_t value) {
  unsigned char encoded[8];

  put_u64le(encoded, value);
  return exfat_hash_bytes(hash, encoded, sizeof(encoded));
}

/* This intentionally hashes source metadata, not source data.  It permits a
 * restart without rereading a large source, while rejecting a renamed, resized,
 * replaced, or modified file before its old stage bytes are reused. */
static uint64_t
exfat_tree_fingerprint_node(const exfat_node_t *node, uint64_t hash) {
  unsigned char kind;
  size_t name_length;

  if(!node) return 0;
  kind = node->is_dir ? 'D' : 'F';
  name_length = strlen(node->name);
  hash = exfat_hash_bytes(hash, &kind, sizeof(kind));
  hash = exfat_hash_u64(hash, (uint64_t)name_length);
  hash = exfat_hash_bytes(hash, node->name, name_length);
  hash = exfat_hash_u64(hash, (uint64_t)node->device);
  hash = exfat_hash_u64(hash, (uint64_t)node->inode);
  hash = exfat_hash_u64(hash, node->size);
  hash = exfat_hash_u64(hash, (uint64_t)node->mtime_sec);
  hash = exfat_hash_u64(hash, (uint64_t)node->mtime_nsec);
  hash = exfat_hash_u64(hash, (uint64_t)node->child_count);
  for(size_t i = 0; i < node->child_count; i++) {
    hash = exfat_tree_fingerprint_node(node->children[i], hash);
  }
  return hash;
}

static uint64_t
exfat_tree_fingerprint(const exfat_node_t *root) {
  return exfat_tree_fingerprint_node(root, UINT64_C(1469598103934665603));
}

static uint64_t
exfat_tree_file_count(const exfat_node_t *node) {
  uint64_t count = node && !node->is_dir ? 1u : 0u;

  if(!node) return 0;
  for(size_t i = 0; i < node->child_count; i++) {
    uint64_t child_count = exfat_tree_file_count(node->children[i]);
    if(UINT64_MAX - count < child_count) return UINT64_MAX;
    count += child_count;
  }
  return count;
}

static int
exfat_tree_file_bytes(const exfat_node_t *node, uint64_t *total) {
  if(!node || !total) return EINVAL;
  if(!node->is_dir) {
    if(node->size > UINT64_MAX - *total) return EOVERFLOW;
    *total += node->size;
  }
  for(size_t i = 0; i < node->child_count; i++) {
    int rc = exfat_tree_file_bytes(node->children[i], total);
    if(rc) return rc;
  }
  return 0;
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

static uint64_t exfat_tree_clusters(const exfat_node_t *node, int root) {
  uint64_t total = exfat_node_clusters(node, root);
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

static int exfat_write_directory(FILE *out, const exfat_node_t *node, int root, uint32_t bitmap_clusters, uint32_t upcase_clusters, uint32_t cluster_count, uint32_t heap_offset) {
  uint64_t capacity = (uint64_t)node->cluster_count * 65536u;
  (void)upcase_clusters;
  uint64_t used = 0;
  uint64_t offset;
  unsigned char special[96];
  if (exfat_cluster_offset(heap_offset, node->first_cluster, &offset) ||
      fseeko(out, (off_t)offset, SEEK_SET) != 0) return EIO;
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

static int exfat_write_directories(FILE *out, const exfat_node_t *node, int root, uint32_t bitmap_clusters, uint32_t upcase_clusters, uint32_t cluster_count, uint32_t heap_offset) {
  for (size_t i = 0; i < node->child_count; i++) {
    const exfat_node_t *child = node->children[i];
    if (child->is_dir) {
      int rc = exfat_write_directory(out, child, 0, bitmap_clusters, upcase_clusters, cluster_count, heap_offset);
      if (rc) return rc;
      rc = exfat_write_directories(out, child, 0, bitmap_clusters, upcase_clusters, cluster_count, heap_offset);
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

static int
exfat_checkpoint(FILE *out, mkpfs_resume_state_t *resume,
                 mkpfs_resume_checkpoint_callback checkpoint,
                 void *checkpoint_opaque) {
  struct stat st;

  if(!out || !resume || !checkpoint) return EINVAL;
  if(fflush(out) != 0 || fsync(fileno(out)) != 0 ||
     fstat(fileno(out), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    return EIO;
  }
  resume->exfat_stage_size = (uint64_t)st.st_size;
  return checkpoint(resume, checkpoint_opaque) ? EIO : 0;
}

static int
exfat_write_file_data(FILE *out, const exfat_node_t *node,
                      const atomic_int *cancel_requested,
                      mkpfs_progress_callback progress, void *opaque,
                      uint64_t *done, uint64_t total, uint32_t heap_offset,
                      unsigned char *buffer, uint64_t *file_index,
                      mkpfs_resume_state_t *resume,
                      mkpfs_resume_checkpoint_callback checkpoint,
                      void *checkpoint_opaque, uint64_t *checkpoint_bytes,
                      uint64_t *checkpoint_files) {
  if (node->is_dir) {
    for (size_t i = 0; i < node->child_count; i++) {
      int rc = exfat_write_file_data(out, node->children[i], cancel_requested,
                                     progress, opaque, done, total, heap_offset,
                                     buffer, file_index, resume, checkpoint,
                                     checkpoint_opaque, checkpoint_bytes,
                                     checkpoint_files);
      if(rc) return rc;
    }
    return 0;
  }
  if(!file_index) return EINVAL;
  if(cancellation_requested(cancel_requested)) return ECANCELED;
  if(resume && *file_index < resume->exfat_next_file) {
    if(node->size > UINT64_MAX - *done) return EOVERFLOW;
    *done += node->size;
    if(UINT64_MAX == *file_index) return EOVERFLOW;
    (*file_index)++;
    return 0;
  }
  if(!node->size) {
    if(UINT64_MAX == *file_index) return EOVERFLOW;
    (*file_index)++;
    if(resume) resume->exfat_next_file = *file_index;
    if(resume && checkpoint && checkpoint_files &&
       *file_index - *checkpoint_files >= EXFAT_CHECKPOINT_FILES) {
      int rc = exfat_checkpoint(out, resume, checkpoint, checkpoint_opaque);
      if(rc) return rc;
      *checkpoint_files = *file_index;
      if(checkpoint_bytes) *checkpoint_bytes = *done;
    }
    return 0;
  }
  struct stat st;
  int fd;
  fd = open(node->path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return errno;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_dev != node->device ||
      st.st_ino != node->inode || (uint64_t)st.st_size != node->size ||
      (int64_t)st.st_mtim.tv_sec != node->mtime_sec ||
      st.st_mtim.tv_nsec != node->mtime_nsec) {
    close(fd);
    return ESTALE;
  }
  FILE *in = fdopen(fd, "rb"); if (!in || !buffer) { if (in) fclose(in); else close(fd); return ENOMEM; }
  uint64_t offset;
  if (exfat_cluster_offset(heap_offset, node->first_cluster, &offset) ||
      fseeko(out, (off_t)offset, SEEK_SET) != 0) { fclose(in); return EIO; }
  uint64_t remaining = node->size;
  while (remaining) {
    if (cancellation_requested(cancel_requested)) { fclose(in); return ECANCELED; }
    size_t want = remaining > 1024 * 1024 ? 1024 * 1024 : (size_t)remaining;
    size_t got = fread(buffer, 1, want, in); if (got != want || write_all(out, buffer, got)) { fclose(in); return EIO; }
    remaining -= got; *done += got;
    if (progress && progress(*done, total, "exfat", node->path, opaque)) { fclose(in); return ECANCELED; }
  }
  {
    int rc = write_zeros(out, (uint64_t)node->cluster_count * 65536u - node->size);
    fclose(in);
    if(rc) return rc;
  }
  if(UINT64_MAX == *file_index) return EOVERFLOW;
  (*file_index)++;
  if(resume) resume->exfat_next_file = *file_index;
  if(resume && checkpoint && checkpoint_bytes && checkpoint_files &&
     (*done - *checkpoint_bytes >= EXFAT_CHECKPOINT_BYTES ||
      *file_index - *checkpoint_files >= EXFAT_CHECKPOINT_FILES)) {
    int rc = exfat_checkpoint(out, resume, checkpoint, checkpoint_opaque);
    if(rc) return rc;
    *checkpoint_bytes = *done;
    *checkpoint_files = *file_index;
  }
  return 0;
}

/* Defined with the generic resumable PFS helpers below. */
static int open_resume_stage(const char *path, FILE **out);
static int remove_private_stage(const char *path);

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

static int
exfat_stage_header_matches(FILE *stage, uint64_t volume_length,
                           uint32_t fat_sectors, uint32_t heap_offset,
                           uint32_t cluster_count, uint32_t root_cluster) {
  unsigned char boot[512];

  if(!stage || fseeko(stage, 0, SEEK_SET) != 0 ||
     read_all(stage, boot, sizeof(boot)) != 0) {
    return 0;
  }
  return boot[0] == 0xEB && boot[1] == 0x76 && boot[2] == 0x90 &&
         !memcmp(boot + 3, "EXFAT   ", 8) &&
         get_u64le(boot + 72) == volume_length &&
         get_u32le(boot + 80) == 128u &&
         get_u32le(boot + 84) == fat_sectors &&
         get_u32le(boot + 88) == heap_offset &&
         get_u32le(boot + 92) == cluster_count &&
         get_u32le(boot + 96) == root_cluster &&
         get_u32le(boot + 100) == 0x4D6B5046u;
}

static int
exfat_write_folder(const char *source, const char *output,
                   const atomic_int *cancel_requested,
                   mkpfs_progress_callback progress, void *opaque,
                   uint64_t *image_size, mkpfs_resume_state_t *resume,
                   mkpfs_resume_checkpoint_callback checkpoint,
                   void *checkpoint_opaque) {
  int error = 0;
  size_t node_count = 0;
  exfat_node_t *root = exfat_scan_tree(source, "/", 1, &error, &node_count, 0);
  FILE *out = NULL;
  unsigned char *fat = NULL;
  unsigned char boot[12 * 512];
  uint32_t bitmap_clusters = 1, upcase_clusters, content_clusters, cluster_count,
           next_cluster, fat_entries, fat_sectors, heap_offset;
  uint64_t content_clusters64, volume_length, total_bytes = 0, done = 0;
  uint64_t file_index = 0, checkpoint_bytes = 0, checkpoint_files = 0;
  uint64_t fingerprint, source_file_count;
  int resumed = 0;
  char temp[PATH_MAX];
  if (!root) return error ? error : EIO;
  upcase_clusters = (MKPFS_EXFAT_UPCASE_SIZE + 65535u) / 65536u;
  content_clusters64 = exfat_tree_clusters(root, 1) + upcase_clusters;
  if (content_clusters64 > UINT32_MAX - 65536u) { exfat_free_tree(root); return EFBIG; }
  content_clusters = (uint32_t)content_clusters64;
  while (bitmap_clusters != (uint32_t)(((bitmap_clusters + content_clusters + 7u) / 8u + 65535u) / 65536u)) bitmap_clusters = (uint32_t)(((bitmap_clusters + content_clusters + 7u) / 8u + 65535u) / 65536u);
  cluster_count = bitmap_clusters + content_clusters;
  next_cluster = 2 + bitmap_clusters + upcase_clusters;
  exfat_assign_clusters(root, 1, &next_cluster);
  fat_entries = cluster_count + 2; fat_sectors = ((fat_entries * 4u + 511u) / 512u + 127u) / 128u * 128u; heap_offset = ((128u + fat_sectors + 127u) / 128u) * 128u; volume_length = (uint64_t)heap_offset + (uint64_t)cluster_count * 128u;
  if((error = exfat_tree_file_bytes(root, &total_bytes)) != 0) {
    exfat_free_tree(root);
    return error;
  }
  fingerprint = exfat_tree_fingerprint(root);
  source_file_count = exfat_tree_file_count(root);
  if(!fingerprint || source_file_count == UINT64_MAX) {
    exfat_free_tree(root);
    return EOVERFLOW;
  }
  if(resume) {
    struct stat stage_st;

    if(!checkpoint || resume->phase != MKPFS_RESUME_EXFAT) {
      exfat_free_tree(root);
      return EINVAL;
    }
    if(resume->exfat_next_file > resume->exfat_file_count ||
       resume->exfat_file_count > source_file_count ||
       (resume->exfat_source_fingerprint &&
        resume->exfat_source_fingerprint != fingerprint) ||
       (resume->exfat_next_file &&
        resume->exfat_file_count != source_file_count)) {
      exfat_free_tree(root);
      return ESTALE;
    }
    if(resume->exfat_next_file) {
      if(lstat(output, &stage_st) || !S_ISREG(stage_st.st_mode) ||
         stage_st.st_nlink != 1 || stage_st.st_size <= 0 ||
         !resume->exfat_stage_size ||
         (uint64_t)stage_st.st_size != resume->exfat_stage_size ||
         (uint64_t)stage_st.st_size > volume_length * 512u) {
        exfat_free_tree(root);
        return ESTALE;
      }
      if((error = open_resume_stage(output, &out)) != 0) {
        exfat_free_tree(root);
        return error;
      }
      if(!exfat_stage_header_matches(out, volume_length, fat_sectors,
                                     heap_offset, cluster_count,
                                     root->first_cluster)) {
        fclose(out);
        exfat_free_tree(root);
        return ESTALE;
      }
      resumed = 1;
      file_index = 0;
      checkpoint_bytes = 0;
      checkpoint_files = resume->exfat_next_file;
    } else {
      if((error = remove_private_stage(output)) != 0 ||
         (error = open_resume_stage(output, &out)) != 0) {
        exfat_free_tree(root);
        return error;
      }
    }
    resume->exfat_size = volume_length * 512u;
    resume->exfat_file_count = source_file_count;
    resume->exfat_source_fingerprint = fingerprint;
  } else if ((error = open_secure_temp_file(output, "exfat", temp, sizeof(temp), &out))) {
    exfat_free_tree(root);
    return error;
  }
  if(!resumed) {
    uint32_t checksum = 0;
    unsigned char *bitmap;

    memset(boot, 0, sizeof(boot)); boot[0] = 0xEB; boot[1] = 0x76;
    boot[2] = 0x90; memcpy(boot + 3, "EXFAT   ", 8);
    put_u64le(boot + 72, volume_length); put_u32le(boot + 80, 128);
    put_u32le(boot + 84, fat_sectors); put_u32le(boot + 88, heap_offset);
    put_u32le(boot + 92, cluster_count); put_u32le(boot + 96, root->first_cluster);
    put_u32le(boot + 100, 0x4D6B5046u); put_u16le(boot + 104, 0x0100);
    boot[108] = 9; boot[109] = 7; boot[110] = 1; boot[111] = 0x80;
    boot[112] = 0xFF; put_u16le(boot + 510, 0xAA55);
    for(int s = 1; s <= 8; s++) put_u32le(boot + s * 512 + 508, 0xAA550000u);
    for(size_t i = 0; i < 11 * 512; i++) {
      if(i != 106 && i != 107 && i != 112) {
        checksum = ((checksum << 31) | (checksum >> 1)) + boot[i];
      }
    }
    for(int i = 11 * 512; i < 12 * 512; i += 4) put_u32le(boot + i, checksum);
    if(write_all(out, boot, sizeof(boot)) || write_all(out, boot, sizeof(boot)) ||
       write_zeros(out, (128u - 24u) * 512u)) {
      error = EIO;
      goto exfat_failed;
    }
    fat = calloc(1, (size_t)fat_sectors * 512u);
    if(!fat) { error = ENOMEM; goto exfat_failed; }
    put_u32le(fat, 0xFFFFFFF8u); put_u32le(fat + 4, 0xFFFFFFFFu);
    exfat_chain(fat, 2, bitmap_clusters);
    exfat_chain(fat, 2 + bitmap_clusters, upcase_clusters);
    exfat_chain_tree(fat, root);
    if(write_all(out, fat, (size_t)fat_sectors * 512u) ||
       write_zeros(out, (uint64_t)(heap_offset - 128u - fat_sectors) * 512u)) {
      error = EIO;
      goto exfat_failed;
    }
    bitmap = calloc(1, (size_t)bitmap_clusters * 65536u);
    if(!bitmap) { error = ENOMEM; goto exfat_failed; }
    memset(bitmap, 0xFF, (size_t)((cluster_count + 7u) / 8u));
    if(write_all(out, bitmap, (size_t)bitmap_clusters * 65536u) ||
       write_all(out, mkpfs_exfat_upcase, MKPFS_EXFAT_UPCASE_SIZE) ||
       write_zeros(out, (uint64_t)upcase_clusters * 65536u -
                        MKPFS_EXFAT_UPCASE_SIZE)) {
      free(bitmap);
      error = EIO;
      goto exfat_failed;
    }
    free(bitmap);
    if(exfat_write_directory(out, root, 1, bitmap_clusters, upcase_clusters,
                             cluster_count, heap_offset) ||
       exfat_write_directories(out, root, 1, bitmap_clusters, upcase_clusters,
                               cluster_count, heap_offset)) {
      error = EIO;
      goto exfat_failed;
    }
    if(resume && (error = exfat_checkpoint(out, resume, checkpoint,
                                           checkpoint_opaque)) != 0) {
      goto exfat_failed;
    }
  }
  unsigned char *file_buffer = (unsigned char *)malloc(1024 * 1024);
  if (!file_buffer) { error = ENOMEM; goto exfat_failed; }
  error = exfat_write_file_data(out, root, cancel_requested, progress, opaque,
                                &done, total_bytes, heap_offset, file_buffer,
                                &file_index, resume, checkpoint,
                                checkpoint_opaque, &checkpoint_bytes,
                                &checkpoint_files);
  free(file_buffer);
  if (error) goto exfat_failed;
  if(resume) {
    resume->exfat_next_file = resume->exfat_file_count;
    if((error = exfat_checkpoint(out, resume, checkpoint,
                                 checkpoint_opaque)) != 0) {
      goto exfat_failed;
    }
  }
  if(fflush(out) != 0 || fsync(fileno(out)) != 0 || fclose(out) != 0) {
    out = NULL;
    error = errno ? errno : EIO;
    if(!resume) unlink(temp);
    goto exfat_done;
  }
  out = NULL;
  if(resume) {
    if((error = checkpoint(resume, checkpoint_opaque)) != 0) {
      error = EIO;
      goto exfat_done;
    }
  } else if(rename(temp, output) != 0) {
    error = errno ? errno : EIO;
    unlink(temp);
    goto exfat_done;
  }
  *image_size = volume_length * 512u; error = 0; goto exfat_done;
exfat_failed:
  if(out) fclose(out);
  if(!resume) unlink(temp);
exfat_done: free(fat); exfat_free_tree(root); return error;
}

int mkpfs_build_exfat_folder(const char *source, const char *output_path, const atomic_int *cancel_requested, mkpfs_progress_callback progress, void *opaque) {
  uint64_t image_size = 0;
  return exfat_write_folder(source, output_path, cancel_requested, progress,
                            opaque, &image_size, NULL, NULL, NULL);
}

int mkpfs_convert_folder_progress(const char *source, const char *destination, const char *output_name, const mkpfs_native_options_t *options, const atomic_int *cancel_requested, mkpfs_progress_callback progress, void *opaque) {
  struct stat st;
  char normalized[PATH_MAX], output[PATH_MAX], exfat_temp[PATH_MAX], inner_name[NAME_MAX + 16];
  int level = options && options->compression_level <= 9 ? (int)options->compression_level : 7;
  uint64_t exfat_size = 0;
  int rc;
  if (!source || !destination || !output_name || !*output_name || strchr(output_name, '/') || strchr(output_name, '\\')) return EINVAL;
  if (mkpfs_normalize_path(source, normalized, sizeof(normalized)) != 0) return EINVAL;
  if (stat(destination, &st) != 0) return errno;
  if (!S_ISDIR(st.st_mode)) return ENOTDIR;
  if (snprintf(output, sizeof(output), "%s/%s", destination, output_name) >= (int)sizeof(output)) return ENAMETOOLONG;
  if (snprintf(exfat_temp, sizeof(exfat_temp), "%s.exfat.tmp.%ld", output, (long)getpid()) >= (int)sizeof(exfat_temp)) return ENAMETOOLONG;
  char title_id[NAME_MAX];
  if (exfat_read_title_id(normalized, title_id, sizeof(title_id)) == 0) {
    if (snprintf(inner_name, sizeof(inner_name), "%s.exfat", title_id) >= (int)sizeof(inner_name)) return ENAMETOOLONG;
  } else if (snprintf(inner_name, sizeof(inner_name), "%s.exfat", output_name) >= (int)sizeof(inner_name)) return ENAMETOOLONG;
  rc = exfat_write_folder(normalized, exfat_temp, cancel_requested, progress,
                          opaque, &exfat_size, NULL, NULL, NULL);
  if (rc) { unlink(exfat_temp); return rc; }
  rc = mkpfs_wrap_exfat_file_ex(exfat_temp, output, inner_name, level, options ? options->workers : 0, cancel_requested, progress, opaque);
  unlink(exfat_temp);
  return rc;
}

int mkpfs_convert_folder(const char *source, const char *destination, const char *output_name, const mkpfs_native_options_t *options, const atomic_int *cancel_requested) {
  return mkpfs_convert_folder_progress(source, destination, output_name, options, cancel_requested, NULL, NULL);
}

static int
resume_path_in_destination(const char *path, const char *destination) {
  size_t length;

  if(!path || !destination || !*path || !*destination) return 0;
  length = strlen(destination);
  if(length == 1 && destination[0] == '/') return path[0] == '/';
  return !strncmp(path, destination, length) && path[length] == '/';
}

static uint64_t
resume_path_hash(const char *value) {
  uint64_t hash = UINT64_C(1469598103934665603);

  for(; *value; value++) {
    hash ^= (unsigned char)*value;
    hash *= UINT64_C(1099511628211);
  }
  hash ^= 0;
  return hash * UINT64_C(1099511628211);
}

static int
resume_stage_paths_match(const mkpfs_resume_state_t *resume,
                         const char *destination, const char *output_name) {
  char output[PATH_MAX];
  char expected_exfat[PATH_MAX];
  char expected_pfs[PATH_MAX];
  uint64_t hash;

  if(!resume || !destination || !output_name ||
     snprintf(output, sizeof(output), "%s/%s", destination, output_name) >=
       (int)sizeof(output)) return 0;
  hash = resume_path_hash(output);
  if(snprintf(expected_exfat, sizeof(expected_exfat),
              "%s/.%s.mkpfs-%016llx.exfat.stage", destination, output_name,
              (unsigned long long)hash) >= (int)sizeof(expected_exfat) ||
     snprintf(expected_pfs, sizeof(expected_pfs),
              "%s/.%s.mkpfs-%016llx.pfs.stage", destination, output_name,
              (unsigned long long)hash) >= (int)sizeof(expected_pfs)) return 0;
  return !strcmp(resume->exfat_path, expected_exfat) &&
         !strcmp(resume->pfs_path, expected_pfs);
}

static int
remove_private_stage(const char *path) {
  struct stat st;

  if(lstat(path, &st) != 0) return errno == ENOENT ? 0 : errno;
  if(!S_ISREG(st.st_mode) || st.st_nlink != 1) return EINVAL;
  return unlink(path) == 0 ? 0 : errno;
}

static int
publish_stage_no_replace(const char *stage_path, const char *output_path) {
  if(link(stage_path, output_path) != 0) return errno;
  if(unlink(stage_path) != 0) {
    int error = errno;
    unlink(output_path);
    return error;
  }
  return 0;
}

int
mkpfs_convert_folder_resumable(
  const char *source, const char *destination, const char *output_name,
  const mkpfs_native_options_t *options, const atomic_int *cancel_requested,
  mkpfs_progress_callback progress, void *progress_opaque,
  mkpfs_resume_state_t *resume,
  mkpfs_resume_checkpoint_callback checkpoint, void *checkpoint_opaque) {
  struct stat st;
  FILE *pfs = NULL;
  char normalized[PATH_MAX];
  char output[PATH_MAX];
  char inner_name[NAME_MAX + 16];
  char title_id[NAME_MAX];
  uint64_t raw_size = 0;
  uint64_t pfsc_size = 0;
  int level;
  int rc;

  if(!source || !destination || !output_name || !*output_name || !resume ||
     !checkpoint || strchr(output_name, '/') || strchr(output_name, '\\')) {
    return EINVAL;
  }
  if(mkpfs_normalize_path(source, normalized, sizeof(normalized)) != 0 ||
     stat(destination, &st) != 0 || !S_ISDIR(st.st_mode) ||
     snprintf(output, sizeof(output), "%s/%s", destination, output_name) >=
       (int)sizeof(output) ||
     !resume_path_in_destination(resume->exfat_path, destination) ||
     !resume_path_in_destination(resume->pfs_path, destination) ||
     !resume_stage_paths_match(resume, destination, output_name) ||
     !strcmp(resume->exfat_path, output) || !strcmp(resume->pfs_path, output) ||
     resume->phase < MKPFS_RESUME_EXFAT ||
     resume->phase > MKPFS_RESUME_PUBLISH) {
    return EINVAL;
  }
  if(lstat(output, &st) == 0) return EEXIST;
  if(errno != ENOENT) return errno;
  level = options && options->compression_level <= 9 ?
          (int)options->compression_level : 7;
  if(resume->phase != MKPFS_RESUME_EXFAT && resume->inner_name[0]) {
    if(!memchr(resume->inner_name, 0, sizeof(resume->inner_name)) ||
       strlen(resume->inner_name) >= sizeof(inner_name)) return EINVAL;
    snprintf(inner_name, sizeof(inner_name), "%s", resume->inner_name);
  } else {
    if(exfat_read_title_id(normalized, title_id, sizeof(title_id)) == 0) {
      if(snprintf(inner_name, sizeof(inner_name), "%s.exfat", title_id) >=
         (int)sizeof(inner_name)) return ENAMETOOLONG;
    } else if(snprintf(inner_name, sizeof(inner_name), "%s.exfat", output_name) >=
              (int)sizeof(inner_name)) {
      return ENAMETOOLONG;
    }
    snprintf(resume->inner_name, sizeof(resume->inner_name), "%s", inner_name);
  }

  if(resume->phase == MKPFS_RESUME_EXFAT) {
    if((rc = exfat_write_folder(normalized, resume->exfat_path,
                                cancel_requested, progress, progress_opaque,
                                &resume->exfat_size, resume, checkpoint,
                                checkpoint_opaque)) != 0) return rc;
    if((rc = remove_private_stage(resume->pfs_path)) != 0) return rc;
    resume->phase = MKPFS_RESUME_PACK;
    resume->pack_next_block = 0;
    resume->pack_stored_size = 0;
    resume->verify_next_block = 0;
    if(checkpoint(resume, checkpoint_opaque)) return EIO;
  }

  if(stat(resume->exfat_path, &st) != 0 || !S_ISREG(st.st_mode) ||
     st.st_nlink != 1 || (resume->exfat_size &&
                          (uint64_t)st.st_size != resume->exfat_size)) {
    return ESTALE;
  }
  resume->exfat_size = (uint64_t)st.st_size;

  if(resume->phase == MKPFS_RESUME_PACK ||
     resume->phase == MKPFS_RESUME_VERIFY ||
     resume->phase == MKPFS_RESUME_PUBLISH) {
    if((rc = open_resume_stage(resume->pfs_path, &pfs)) != 0) return rc;
    if(!pfs) return EIO;
    if(resume->phase == MKPFS_RESUME_PACK) {
      rc = pack_pfsc_into_stream(resume->exfat_path, pfs, 6u * 65536u, level,
                                 options ? options->workers : 0,
                                 cancel_requested, progress, progress_opaque,
                                 &pfsc_size, resume, checkpoint,
                                 checkpoint_opaque);
      if(rc != 0) goto done;
    }
    if(resume->phase == MKPFS_RESUME_PUBLISH) {
      /* Publication metadata may have been interrupted after the packed bytes
       * were durable. Re-verify from block zero before writing it again. */
      resume->phase = MKPFS_RESUME_VERIFY;
      resume->verify_next_block = 0;
      if(checkpoint(resume, checkpoint_opaque)) {
        rc = EIO;
        goto done;
      }
    }
    if(resume->phase == MKPFS_RESUME_VERIFY) {
      rc = verify_pfsc_stream(pfs, 6u * 65536u, &raw_size, NULL,
                              cancel_requested, progress, progress_opaque,
                              resume, checkpoint, checkpoint_opaque);
      if(rc != 0) goto done;
    }
    if(resume->phase != MKPFS_RESUME_PUBLISH) {
      rc = EINVAL;
      goto done;
    }
    if(resume->pack_stored_size < PFSC_INITIAL_DATA_OFFSET ||
       resume->pack_stored_size > UINT64_MAX - 6u * 65536u ||
       ftruncate(fileno(pfs), (off_t)(6u * 65536u +
                                      resume->pack_stored_size)) != 0 ||
       fseeko(pfs, 0, SEEK_END) != 0 || ftello(pfs) < 0) {
      rc = EIO;
      goto done;
    }
    pfsc_size = (uint64_t)ftello(pfs) - 6u * 65536u;
    rc = publish_resumable_pfs(pfs, output, inner_name, pfsc_size, raw_size,
                               cancel_requested, progress, progress_opaque);
    if(rc != 0) goto done;
    if(fclose(pfs) != 0) {
      pfs = NULL;
      return EIO;
    }
    pfs = NULL;
    if((rc = publish_stage_no_replace(resume->pfs_path, output)) != 0) return rc;
    if((rc = remove_private_stage(resume->exfat_path)) != 0) return rc;
    resume->phase = MKPFS_RESUME_DONE;
    if(checkpoint(resume, checkpoint_opaque)) return EIO;
    return 0;
  }

  return EINVAL;
done:
  if(pfs && fclose(pfs) != 0 && rc == 0) rc = EIO;
  return rc;
}
