#include "filemgr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef __linux__
#include <ps5/kernel.h>
#endif

#include "filemgr_internal.h"
#include "archive_extract.h"
#include "mkpfs_native.h"
#include "json_util.h"
#include "path_util.h"
#include "pkg_info.h"
#include "pkg_installer.h"
#include "websrv.h"

#define COPY_BUFFER_SIZE (8 * 1024 * 1024)
#define COPY_PIPELINE_SLOTS 3
#define FILEMGR_AGGRESSIVE_COPY 0
#define FILEMGR_PIPELINE_COPY 1
#define SMALL_COPY_WORKERS 3
#define FILE_TASK_QUEUE_LIMIT 128
#define LARGE_FILE_THRESHOLD (256LL * 1024 * 1024)
#define TRANSFER_ALERT_THRESHOLD (10 * 60)

#ifndef __linux__
typedef struct shell_ui_uri_param {
  uint32_t size;
  uint32_t user_id;
} shell_ui_uri_param_t;

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t,
                             void *, int *);
int sceUserServiceInitialize(const int *);
int sceUserServiceGetForegroundUser(int *);

static int
navigate_to_home(void) {
  int (*initialize)(void);
  int (*launch_by_uri)(const char *, shell_ui_uri_param_t *);
  shell_ui_uri_param_t param = {.size = sizeof(param)};
  const char *module_path =
    "/system_ex/common_ex/lib/libSceShellUIUtil.sprx";
  const char *uri = "pshomeui:navigateToHome?bootCondition=psButton";
  const int priority = 256;
  int module;
  int result;

  (void)sceUserServiceInitialize(&priority);
  module = sceKernelLoadStartModule(module_path, 0, NULL, 0, NULL, NULL);
  if(module < 0) {
    printf("load libSceShellUIUtil: 0x%08X\n", (unsigned int)module);
    return module;
  }
  initialize = (void *)kernel_dynlib_dlsym(
    -1, (uint32_t)module, "sceShellUIUtilInitialize");
  launch_by_uri = (void *)kernel_dynlib_dlsym(
    -1, (uint32_t)module, "sceShellUIUtilLaunchByUri");
  if(!initialize || !launch_by_uri) {
    printf("resolve libSceShellUIUtil URI functions failed\n");
    return -1;
  }

  result = initialize();
  if(result < 0) {
    printf("sceShellUIUtilInitialize: 0x%08X\n", (unsigned int)result);
  }
  result = sceUserServiceGetForegroundUser((int *)&param.user_id);
  if(result < 0) {
    printf("sceUserServiceGetForegroundUser: 0x%08X\n",
           (unsigned int)result);
  }
  result = launch_by_uri(uri, &param);
  printf("sceShellUIUtilLaunchByUri: 0x%08X\n", (unsigned int)result);
  return result;
}
#endif

typedef struct task_completion {
  unsigned long id;
  task_op_t op;
  char src[PATH_MAX];
  size_t src_count;
  unsigned long long total;
  size_t file_count;
  time_t elapsed;
} task_completion_t;

static int ensure_copy_dir(const char *path);
static int open_copy_temp(const char *dst, char *temp, size_t temp_size);
#if FILEMGR_PIPELINE_COPY
typedef struct copy_pipeline_slot {
  char *data;
  size_t size;
  int ready;
} copy_pipeline_slot_t;

typedef struct copy_pipeline {
  file_task_t *task;
  int in;
  copy_pipeline_slot_t slots[COPY_PIPELINE_SLOTS];
  pthread_mutex_t lock;
  pthread_cond_t can_read;
  pthread_cond_t can_write;
  int read_index;
  int write_index;
  int done;
  int error;
  int error_number;
} copy_pipeline_t;
#endif

#if FILEMGR_AGGRESSIVE_COPY
typedef struct copy_job {
  char src[PATH_MAX];
  char dst[PATH_MAX];
  struct copy_job *next;
} copy_job_t;

typedef struct copy_queue {
  file_task_t *task;
  pthread_mutex_t lock;
  pthread_cond_t has_work;
  pthread_cond_t has_space;
  pthread_cond_t idle;
  pthread_t workers[SMALL_COPY_WORKERS];
  copy_job_t *head;
  copy_job_t *tail;
  int queued;
  int active;
  int stopping;
  int error;
  int error_number;
  int worker_count;
} copy_queue_t;
#endif

static task_completion_t g_last_completion;
#ifndef __linux__
static pthread_cond_t g_pkg_tasks_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_pkg_worker_thread;
static int g_pkg_worker_started;
#endif

void
record_task_completion_locked(file_task_t *task, time_t completed_at) {
  if((task->op == TASK_COPY || task->op == TASK_MOVE || task->op == TASK_UPLOAD) &&
     completed_at - task->created_at >= TRANSFER_ALERT_THRESHOLD) {
    g_last_completion.id = task->id;
    g_last_completion.op = task->op;
    snprintf(g_last_completion.src, sizeof(g_last_completion.src), "%s", task->src);
    g_last_completion.src_count = task->src_count;
    g_last_completion.total = task->total;
    g_last_completion.file_count = task->dir_count ? task->file_count :
                                   task->op == TASK_UPLOAD ? task->file_count : 0;
    g_last_completion.elapsed = completed_at - task->created_at;
  }
}

int
ensure_parent_dirs(const char *base, const char *rel) {
  char current[PATH_MAX];
  const char *p = rel;
  size_t base_len;

  if(!relative_path_safe(rel)) {
    return -1;
  }
  if(strlen(base) >= sizeof(current)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(current, base);
  base_len = strlen(current);
  while(*p) {
    const char *slash = strchr(p, '/');
    size_t len;

    if(!slash) {
      return 0;
    }
    len = (size_t)(slash - rel);
    if(base_len + (strcmp(base, "/") ? 1 : 0) + len >= sizeof(current)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    snprintf(current, sizeof(current), "%s%s%.*s", base,
             strcmp(base, "/") ? "/" : "", (int)len, rel);
    if(ensure_copy_dir(current)) {
      return -1;
    }
    p = slash + 1;
  }
  return 0;
}

static void
task_set_error_code(file_task_t *task, const char *code, const char *arg) {
  pthread_mutex_lock(&g_tasks_lock);
  if(code) {
    snprintf(task->error_code, sizeof(task->error_code), "%s", code);
  }
  if(arg) {
    snprintf(task->error_arg, sizeof(task->error_arg), "%s", arg);
  }
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
task_set_total(file_task_t *task, unsigned long long total) {
  pthread_mutex_lock(&g_tasks_lock);
  task->total = total;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
task_finish_bytes(file_task_t *task, const char *current) {
  pthread_mutex_lock(&g_tasks_lock);
  task->state = TASK_RUNNING;
  if(current) {
    snprintf(task->current, sizeof(task->current), "%s", current);
  }
  if(task->total) {
    task->done = task->total;
  }
  task->speed = 0;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

enum MHD_Result
send_buffer(struct MHD_Connection *conn, unsigned int status, char *data,
            const char *mime) {
  struct MHD_Response *resp;
  enum MHD_Result ret = MHD_NO;
  size_t len = data ? strlen(data) : 0;

  if((resp = MHD_create_response_from_buffer(len, data ? data : "",
                                             data ? MHD_RESPMEM_MUST_FREE :
                                                    MHD_RESPMEM_PERSISTENT))) {
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else {
    free(data);
  }

  return ret;
}

enum MHD_Result
send_json_ok(struct MHD_Connection *conn) {
  return send_buffer(conn, MHD_HTTP_OK, strdup("{\"ok\":true}"),
                     "application/json");
}

static const char *
api_error_code(const char *msg) {
  if(!msg) return "system_error";
  if(!strcmp(msg, "another task is running")) return "active_task";
  if(!strcmp(msg, "active task not found")) return "active_task_not_found";
  if(!strcmp(msg, "source and destination are the same")) return "source_destination_same";
  if(!strcmp(msg, "destination is inside source directory")) return "destination_inside_source";
  if(!strcmp(msg, "invalid path")) return "invalid_path";
  if(!strcmp(msg, "file not found")) return "file_not_found";
  if(!strcmp(msg, "invalid method")) return "invalid_method";
  if(!strcmp(msg, "unknown api")) return "unknown_api";
  if(!strcmp(msg, "out of memory")) return "out_of_memory";
  if(!strcmp(msg, "no source paths")) return "no_source_paths";
  if(!strcmp(msg, "file type is not editable")) return "text_type_not_editable";
  if(!strcmp(msg, "text file is too large")) return "text_file_too_large";
  if(!strcmp(msg, "file is not valid UTF-8")) return "text_invalid_utf8";
  if(!strcmp(msg, "file changed since it was opened")) return "text_file_changed";
  if(!strcmp(msg, "text file is not writable")) return "text_file_not_writable";
  if(!strcmp(msg, "file already exists")) return "file_already_exists";
  if(!strcmp(msg, "destination must be a directory for multiple items")) {
    return "destination_must_be_directory";
  }
  return "system_error";
}

enum MHD_Result
send_json_error(struct MHD_Connection *conn, unsigned int status,
                const char *msg) {
  return send_json_error_detail(conn, status, msg, api_error_code(msg), NULL);
}

enum MHD_Result
send_json_error_detail(struct MHD_Connection *conn, unsigned int status,
                       const char *msg, const char *code, const char *arg) {
  strbuf_t b = {0};
  const char *fallback = msg ? msg : strerror(errno);

  strbuf_append(&b, "{\"ok\":false,\"error\":");
  json_escape(&b, fallback);
  strbuf_append(&b, ",\"error_code\":");
  json_escape(&b, code ? code : api_error_code(msg));
  strbuf_append(&b, ",\"error_arg\":");
  json_escape(&b, arg ? arg : fallback);
  strbuf_append(&b, "}");
  return send_buffer(conn, status, b.data, "application/json");
}

static int task_target_path(file_task_t *task, const char *src,
                            char *out, size_t size);
static int chmod_task_path(file_task_t *task, const char *path,
                           unsigned int mode, int recursive);

static int count_path_bytes_sync(file_task_t *task, const char *path,
                                 const char *display,
                                 unsigned long long *total,
                                 size_t *file_count, size_t *dir_count);

static int
count_dir_bytes_sync(file_task_t *task, const char *path, const char *display,
                     unsigned long long *total, size_t *file_count,
                     size_t *dir_count) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int ret = -1;

  if(!dir) {
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       count_path_bytes_sync(task, child, display ? display_child : NULL,
                             total, file_count, dir_count)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
count_path_bytes_sync(file_task_t *task, const char *path, const char *display,
                      unsigned long long *total, size_t *file_count,
                      size_t *dir_count) {
  struct stat st;

  if(!path || !total) {
    errno = EINVAL;
    return -1;
  }
  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    if(dir_count) (*dir_count)++;
    return count_dir_bytes_sync(task, path, display, total, file_count,
                                dir_count);
  }
  if(S_ISREG(st.st_mode)) {
    *total += (unsigned long long)st.st_size;
    if(file_count) (*file_count)++;
  }
  return 0;
}

#if FILEMGR_AGGRESSIVE_COPY
typedef struct count_job {
  char path[PATH_MAX];
  char display[PATH_MAX];
  int has_display;
  struct count_job *next;
} count_job_t;

typedef struct count_queue {
  file_task_t *task;
  pthread_mutex_t lock;
  pthread_cond_t has_work;
  pthread_cond_t has_space;
  pthread_cond_t idle;
  pthread_t workers[SMALL_COPY_WORKERS];
  count_job_t *head;
  count_job_t *tail;
  int queued;
  int active;
  int stopping;
  int error;
  int error_number;
  int worker_count;
  unsigned long long total;
  size_t file_count;
  size_t dir_count;
} count_queue_t;

static void
count_queue_add(count_queue_t *queue, unsigned long long total, size_t file_count,
                size_t dir_count) {
  pthread_mutex_lock(&queue->lock);
  queue->total += total;
  queue->file_count += file_count;
  queue->dir_count += dir_count;
  pthread_mutex_unlock(&queue->lock);
}

static void *
count_queue_worker(void *arg) {
  count_queue_t *queue = arg;

  for(;;) {
    count_job_t *job;
    unsigned long long total = 0;
    size_t file_count = 0;
    size_t dir_count = 0;
    int ret;

    pthread_mutex_lock(&queue->lock);
    while(!queue->stopping && !queue->head) {
      pthread_cond_wait(&queue->has_work, &queue->lock);
    }
    if(queue->stopping && !queue->head) {
      pthread_mutex_unlock(&queue->lock);
      return NULL;
    }
    job = queue->head;
    queue->head = job->next;
    if(!queue->head) {
      queue->tail = NULL;
    }
    queue->queued--;
    queue->active++;
    pthread_cond_signal(&queue->has_space);
    pthread_mutex_unlock(&queue->lock);

    ret = count_path_bytes_sync(queue->task, job->path,
                                job->has_display ? job->display : NULL,
                                &total, &file_count, &dir_count);
    if(!ret) {
      count_queue_add(queue, total, file_count, dir_count);
    }

    pthread_mutex_lock(&queue->lock);
    if(ret) {
      queue->error = 1;
      queue->error_number = errno ? errno : EIO;
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
    }
    queue->active--;
    if(!queue->head && !queue->active) {
      pthread_cond_broadcast(&queue->idle);
    }
    pthread_mutex_unlock(&queue->lock);
    free(job);
  }
}

static int
count_queue_init(count_queue_t *queue, file_task_t *task) {
  int i;

  memset(queue, 0, sizeof(*queue));
  queue->task = task;
  if(pthread_mutex_init(&queue->lock, NULL)) {
    return -1;
  }
  if(pthread_cond_init(&queue->has_work, NULL)) {
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->has_space, NULL)) {
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->idle, NULL)) {
    pthread_cond_destroy(&queue->has_space);
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  for(i = 0; i < SMALL_COPY_WORKERS; i++) {
    if(pthread_create(&queue->workers[i], NULL, count_queue_worker, queue)) {
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      while(queue->worker_count > 0) {
        pthread_join(queue->workers[--queue->worker_count], NULL);
      }
      pthread_cond_destroy(&queue->idle);
      pthread_cond_destroy(&queue->has_space);
      pthread_cond_destroy(&queue->has_work);
      pthread_mutex_destroy(&queue->lock);
      return -1;
    }
    queue->worker_count++;
  }
  return 0;
}

static int
count_queue_enqueue(count_queue_t *queue, const char *path, const char *display) {
  count_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->path, sizeof(job->path), "%s", path);
  if(display) {
    snprintf(job->display, sizeof(job->display), "%s", display);
    job->has_display = 1;
  }

  pthread_mutex_lock(&queue->lock);
  while(!queue->stopping && queue->queued >= FILE_TASK_QUEUE_LIMIT) {
    pthread_cond_wait(&queue->has_space, &queue->lock);
  }
  if(queue->stopping || queue->error || task_cancel_requested(queue->task)) {
    pthread_mutex_unlock(&queue->lock);
    free(job);
    errno = queue->error_number ? queue->error_number : ECANCELED;
    return -1;
  }
  if(queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  queue->queued++;
  pthread_cond_signal(&queue->has_work);
  pthread_mutex_unlock(&queue->lock);
  return 0;
}

static int
count_queue_finish(count_queue_t *queue, int abort_pending) {
  count_job_t *job;
  int ret = abort_pending ? -1 : 0;
  int i;

  pthread_mutex_lock(&queue->lock);
  while(!abort_pending && !queue->error && (queue->head || queue->active)) {
    pthread_cond_wait(&queue->idle, &queue->lock);
  }
  if(queue->error) {
    errno = queue->error_number ? queue->error_number : EIO;
    ret = -1;
  }
  queue->stopping = 1;
  if(abort_pending || ret) {
    while(queue->head) {
      job = queue->head;
      queue->head = job->next;
      free(job);
    }
    queue->tail = NULL;
    queue->queued = 0;
  }
  pthread_cond_broadcast(&queue->has_work);
  pthread_cond_broadcast(&queue->has_space);
  pthread_mutex_unlock(&queue->lock);

  for(i = 0; i < queue->worker_count; i++) {
    pthread_join(queue->workers[i], NULL);
  }
  while(queue->head) {
    job = queue->head;
    queue->head = job->next;
    free(job);
  }
  pthread_cond_destroy(&queue->idle);
  pthread_cond_destroy(&queue->has_space);
  pthread_cond_destroy(&queue->has_work);
  pthread_mutex_destroy(&queue->lock);
  return ret;
}

static int
count_path_bytes(file_task_t *task, const char *path, const char *display,
                 unsigned long long *total, size_t *file_count,
                 size_t *dir_count) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  int ret = -1;
  int queue_finished = 0;
  count_queue_t queue;

  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    return count_path_bytes_sync(task, path, display, total, file_count,
                                 dir_count);
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }
  if(dir_count) (*dir_count)++;
  if(count_queue_init(&queue, task)) {
    return -1;
  }
  if(!(dir = opendir(path))) {
    count_queue_finish(&queue, 1);
    return -1;
  }

  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      errno = ECANCELED;
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       count_queue_enqueue(&queue, child, display ? display_child : NULL)) {
      goto done;
    }
  }

  ret = count_queue_finish(&queue, 0);
  queue_finished = 1;
  if(!ret) {
    *total += queue.total;
    if(file_count) {
      *file_count += queue.file_count;
    }
    if(dir_count) {
      *dir_count += queue.dir_count;
    }
  }
done:
  closedir(dir);
  if(ret && !queue_finished) {
    count_queue_finish(&queue, 1);
  }
  return ret;
}
#else
#define count_path_bytes count_path_bytes_sync
#endif

int
count_task_path_bytes(file_task_t *task, const char *path, const char *display,
                      unsigned long long *total, size_t *file_count,
                      size_t *dir_count) {
  return count_path_bytes(task, path, display, total, file_count, dir_count);
}

static int
open_copy_temp(const char *dst, char *temp, size_t temp_size) {
  char parent[PATH_MAX];
  char name[96];
  int attempt;

  if(path_dirname(dst, parent, sizeof(parent))) {
    return -1;
  }
  for(attempt = 0; attempt < 32; attempt++) {
    int fd;

    snprintf(name, sizeof(name), ".wfm-copy-%ld-%lld-%d.tmp",
             (long)getpid(), (long long)time(NULL), attempt);
    if(path_join(temp, temp_size, parent, name)) {
      return -1;
    }
    fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd >= 0) {
      return fd;
    }
    if(errno != EEXIST) {
      return -1;
    }
  }
  errno = EEXIST;
  return -1;
}

static int
copy_file_buffered(file_task_t *task, const char *src, const char *dst) {
  char *buf = NULL;
  char temp[PATH_MAX] = {0};
  int in = -1;
  int out = -1;
  int ret = -1;
  ssize_t n;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    return -1;
  }
  if((in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if((out = open_copy_temp(dst, temp, sizeof(temp))) < 0) {
    goto done;
  }
  if(!(buf = malloc(COPY_BUFFER_SIZE))) {
    errno = ENOMEM;
    goto done;
  }

  while((n = read(in, buf, COPY_BUFFER_SIZE)) > 0) {
    ssize_t left = n;
    char *p = buf;
    if(task_cancel_requested(task)) {
      errno = ECANCELED;
      goto done;
    }
    while(left > 0) {
      ssize_t w = write(out, p, (size_t)left);
      if(w <= 0) {
        goto done;
      }
      p += w;
      left -= w;
      task_update(task, TASK_RUNNING, dst, (unsigned long long)w, NULL);
    }
  }
  if(n < 0) {
    goto done;
  }
  if(fchmod_0777(out)) {
    goto done;
  }
  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    goto done;
  }
  ret = 0;

done:
  free(buf);
  if(in >= 0) close(in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(!ret && task_cancel_requested(task)) {
    errno = ECANCELED;
    ret = -1;
  }
  if(!ret && rename(temp, dst)) {
    ret = -1;
  }
  if(ret && temp[0]) {
    unlink(temp);
  }
  return ret;
}

#if FILEMGR_PIPELINE_COPY
static void
pipeline_fail_locked(copy_pipeline_t *p, int error) {
  p->error = 1;
  p->done = 1;
  p->error_number = error ? error : EIO;
  pthread_cond_broadcast(&p->can_read);
  pthread_cond_broadcast(&p->can_write);
}

static void *
copy_pipeline_reader(void *arg) {
  copy_pipeline_t *p = arg;

  for(;;) {
    int slot;
    ssize_t n;

    pthread_mutex_lock(&p->lock);
    while(!p->error && !p->done && p->slots[p->read_index].ready) {
      pthread_cond_wait(&p->can_read, &p->lock);
    }
    if(p->error || p->done || task_cancel_requested(p->task)) {
      p->done = 1;
      pthread_cond_broadcast(&p->can_write);
      pthread_mutex_unlock(&p->lock);
      return NULL;
    }
    slot = p->read_index;
    p->read_index = (p->read_index + 1) % COPY_PIPELINE_SLOTS;
    pthread_mutex_unlock(&p->lock);

    n = read(p->in, p->slots[slot].data, COPY_BUFFER_SIZE);

    pthread_mutex_lock(&p->lock);
    if(n < 0) {
      pipeline_fail_locked(p, errno);
    } else if(n == 0) {
      p->done = 1;
      pthread_cond_broadcast(&p->can_write);
    } else {
      p->slots[slot].size = (size_t)n;
      p->slots[slot].ready = 1;
      pthread_cond_signal(&p->can_write);
    }
    pthread_mutex_unlock(&p->lock);
  }
}

static int
copy_file_pipeline(file_task_t *task, const char *src, const char *dst) {
  copy_pipeline_t p;
  pthread_t reader;
  char temp[PATH_MAX] = {0};
  int out = -1;
  int ret = -1;
  int reader_started = 0;
  int lock_ready = 0;
  int can_read_ready = 0;
  int can_write_ready = 0;
  int i;

  memset(&p, 0, sizeof(p));
  p.task = task;
  p.in = -1;
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    return -1;
  }
  if((p.in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if((out = open_copy_temp(dst, temp, sizeof(temp))) < 0) {
    goto done;
  }
  if(pthread_mutex_init(&p.lock, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  lock_ready = 1;
  if(pthread_cond_init(&p.can_read, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  can_read_ready = 1;
  if(pthread_cond_init(&p.can_write, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  can_write_ready = 1;
  for(i = 0; i < COPY_PIPELINE_SLOTS; i++) {
    if(posix_memalign((void **)&p.slots[i].data, 4096, COPY_BUFFER_SIZE)) {
      errno = ENOMEM;
      goto done;
    }
  }
  if(pthread_create(&reader, NULL, copy_pipeline_reader, &p)) {
    errno = EAGAIN;
    goto done;
  }
  reader_started = 1;

  for(;;) {
    int slot;
    char *buf;
    size_t size;
    size_t off = 0;

    pthread_mutex_lock(&p.lock);
    while(!p.error && !p.done && !p.slots[p.write_index].ready) {
      pthread_cond_wait(&p.can_write, &p.lock);
    }
    if(p.error) {
      errno = p.error_number;
      pthread_mutex_unlock(&p.lock);
      goto done;
    }
    if(p.done && !p.slots[p.write_index].ready) {
      pthread_mutex_unlock(&p.lock);
      break;
    }
    slot = p.write_index;
    buf = p.slots[slot].data;
    size = p.slots[slot].size;
    pthread_mutex_unlock(&p.lock);

    while(off < size) {
      ssize_t n;
      if(task_cancel_requested(task)) {
        errno = ECANCELED;
        goto done;
      }
      n = write(out, buf + off, size - off);
      if(n <= 0) {
        if(!n) errno = EIO;
        goto done;
      }
      off += (size_t)n;
      task_update(task, TASK_RUNNING, dst, (unsigned long long)n, NULL);
    }

    pthread_mutex_lock(&p.lock);
    p.slots[slot].ready = 0;
    p.write_index = (p.write_index + 1) % COPY_PIPELINE_SLOTS;
    pthread_cond_signal(&p.can_read);
    pthread_mutex_unlock(&p.lock);
  }

  if(fchmod_0777(out)) {
    goto done;
  }
  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    goto done;
  }
  ret = 0;

done:
  if(reader_started) {
    pthread_mutex_lock(&p.lock);
    p.done = 1;
    p.error = 1;
    pthread_cond_broadcast(&p.can_read);
    pthread_cond_broadcast(&p.can_write);
    pthread_mutex_unlock(&p.lock);
    pthread_join(reader, NULL);
  }
  for(i = 0; i < COPY_PIPELINE_SLOTS; i++) {
    free(p.slots[i].data);
  }
  if(can_write_ready) pthread_cond_destroy(&p.can_write);
  if(can_read_ready) pthread_cond_destroy(&p.can_read);
  if(lock_ready) pthread_mutex_destroy(&p.lock);
  if(p.in >= 0) close(p.in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(!ret && task_cancel_requested(task)) {
    errno = ECANCELED;
    ret = -1;
  }
  if(!ret && rename(temp, dst)) {
    ret = -1;
  }
  if(ret && temp[0]) {
    unlink(temp);
  }
  return ret;
}
#endif

static int
copy_file(file_task_t *task, const char *src, const char *dst) {
#if FILEMGR_PIPELINE_COPY
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(st.st_size >= LARGE_FILE_THRESHOLD) {
    return copy_file_pipeline(task, src, dst);
  }
#endif
  return copy_file_buffered(task, src, dst);
}

static int copy_path(file_task_t *task, const char *src, const char *dst);
static int remove_path(file_task_t *task, const char *path);
static int check_remove_path_writable(file_task_t *task, const char *path);
static int
ensure_copy_dir(const char *path) {
  struct stat st;

  if(mkdir(path, 0777)) {
    if(errno != EEXIST) {
      return -1;
    }
    if(lstat(path, &st)) {
      return -1;
    }
    if(!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
  }
  return chmod_path_0777(path);
}

static int
check_remove_entry_writable(const char *path) {
  char parent[PATH_MAX];

  if(path_dirname(path, parent, sizeof(parent))) {
    return -1;
  }
  return mode_access(parent, W_OK | X_OK);
}

static int
check_remove_dir_writable(file_task_t *task, const char *path) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  if(mode_access(path, R_OK | W_OK | X_OK)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(!(dir = opendir(path))) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       check_remove_path_writable(task, child)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
check_remove_path_writable(file_task_t *task, const char *path) {
  struct stat st;

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(check_remove_entry_writable(path)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return check_remove_dir_writable(task, path);
  }
  return 0;
}

static int
finish_copied_move(file_task_t *task, const char *src) {
  int ret;

  task_finish_bytes(task, "checking source permissions");
  ret = check_remove_path_writable(task, src);
  if(!ret) {
    task_finish_bytes(task, "removing source");
    ret = remove_path(task, src);
  }
  return ret;
}

#if FILEMGR_AGGRESSIVE_COPY
static int copy_dir_queued(file_task_t *task, const char *src, const char *dst,
                           copy_queue_t *queue);

static void *
copy_queue_worker(void *arg) {
  copy_queue_t *queue = arg;

  for(;;) {
    copy_job_t *job;
    int ret;

    pthread_mutex_lock(&queue->lock);
    while(!queue->stopping && !queue->head) {
      pthread_cond_wait(&queue->has_work, &queue->lock);
    }
    if(queue->stopping && !queue->head) {
      pthread_mutex_unlock(&queue->lock);
      return NULL;
    }
    job = queue->head;
    queue->head = job->next;
    if(!queue->head) {
      queue->tail = NULL;
    }
    queue->queued--;
    queue->active++;
    pthread_cond_signal(&queue->has_space);
    pthread_mutex_unlock(&queue->lock);

    ret = task_cancel_requested(queue->task) ? -1 :
      copy_file_buffered(queue->task, job->src, job->dst);
    if(ret && task_cancel_requested(queue->task)) {
      errno = ECANCELED;
    }

    pthread_mutex_lock(&queue->lock);
    if(ret) {
      queue->error = 1;
      queue->error_number = errno ? errno : EIO;
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      pthread_cond_broadcast(&queue->has_space);
    }
    queue->active--;
    if(!queue->head && !queue->active) {
      pthread_cond_broadcast(&queue->idle);
    }
    pthread_mutex_unlock(&queue->lock);
    free(job);
  }
}

static int
copy_queue_init(copy_queue_t *queue, file_task_t *task) {
  int i;

  memset(queue, 0, sizeof(*queue));
  queue->task = task;
  if(pthread_mutex_init(&queue->lock, NULL)) {
    return -1;
  }
  if(pthread_cond_init(&queue->has_work, NULL)) {
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->has_space, NULL)) {
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->idle, NULL)) {
    pthread_cond_destroy(&queue->has_space);
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  for(i = 0; i < SMALL_COPY_WORKERS; i++) {
    if(pthread_create(&queue->workers[i], NULL, copy_queue_worker, queue)) {
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      while(queue->worker_count > 0) {
        pthread_join(queue->workers[--queue->worker_count], NULL);
      }
      pthread_cond_destroy(&queue->idle);
      pthread_cond_destroy(&queue->has_space);
      pthread_cond_destroy(&queue->has_work);
      pthread_mutex_destroy(&queue->lock);
      return -1;
    }
    queue->worker_count++;
  }
  return 0;
}

static int
copy_queue_enqueue(copy_queue_t *queue, const char *src, const char *dst) {
  copy_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->src, sizeof(job->src), "%s", src);
  snprintf(job->dst, sizeof(job->dst), "%s", dst);

  pthread_mutex_lock(&queue->lock);
  while(!queue->stopping && queue->queued >= FILE_TASK_QUEUE_LIMIT) {
    pthread_cond_wait(&queue->has_space, &queue->lock);
  }
  if(queue->stopping || queue->error || task_cancel_requested(queue->task)) {
    pthread_mutex_unlock(&queue->lock);
    free(job);
    errno = queue->error_number ? queue->error_number : ECANCELED;
    return -1;
  }
  if(queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  queue->queued++;
  pthread_cond_signal(&queue->has_work);
  pthread_mutex_unlock(&queue->lock);
  return 0;
}

static int
copy_queue_wait(copy_queue_t *queue) {
  int ret = 0;

  pthread_mutex_lock(&queue->lock);
  while(!queue->error && (queue->head || queue->active)) {
    pthread_cond_wait(&queue->idle, &queue->lock);
  }
  if(queue->error) {
    errno = queue->error_number ? queue->error_number : EIO;
    ret = -1;
  }
  pthread_mutex_unlock(&queue->lock);
  return ret;
}

static int
copy_queue_finish(copy_queue_t *queue, int abort_pending) {
  copy_job_t *job;
  int ret = abort_pending ? -1 : copy_queue_wait(queue);
  int i;

  pthread_mutex_lock(&queue->lock);
  queue->stopping = 1;
  if(abort_pending) {
    while(queue->head) {
      job = queue->head;
      queue->head = job->next;
      free(job);
    }
    queue->tail = NULL;
    queue->queued = 0;
  }
  pthread_cond_broadcast(&queue->has_work);
  pthread_cond_broadcast(&queue->has_space);
  pthread_mutex_unlock(&queue->lock);
  for(i = 0; i < queue->worker_count; i++) {
    pthread_join(queue->workers[i], NULL);
  }
  while(queue->head) {
    job = queue->head;
    queue->head = job->next;
    free(job);
  }
  pthread_cond_destroy(&queue->idle);
  pthread_cond_destroy(&queue->has_space);
  pthread_cond_destroy(&queue->has_work);
  pthread_mutex_destroy(&queue->lock);
  return ret;
}

static int
copy_dir(file_task_t *task, const char *src, const char *dst) {
  copy_queue_t queue;
  int ret;

  if(copy_queue_init(&queue, task)) {
    return -1;
  }
  ret = copy_dir_queued(task, src, dst, &queue);
  if(copy_queue_finish(&queue, ret)) {
    ret = -1;
  }
  return ret;
}

static int
copy_dir_queued(file_task_t *task, const char *src, const char *dst,
                copy_queue_t *queue) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  int ret = -1;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(ensure_copy_dir(dst)) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while((entry = readdir(dir))) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(from, sizeof(from), src, entry->d_name) ||
       path_join(to, sizeof(to), dst, entry->d_name)) {
      goto done;
    }
    if(lstat(from, &st)) {
      goto done;
    }
    if(S_ISDIR(st.st_mode)) {
      if(copy_dir_queued(task, from, to, queue)) {
        goto done;
      }
    } else if(S_ISREG(st.st_mode)) {
#if FILEMGR_PIPELINE_COPY
      if(st.st_size >= LARGE_FILE_THRESHOLD) {
        if(copy_queue_wait(queue) || copy_file_pipeline(task, from, to)) {
          goto done;
        }
      } else
#endif
      if(copy_queue_enqueue(queue, from, to)) {
        goto done;
      }
    } else {
      errno = ENOTSUP;
      goto done;
    }
  }

  ret = 0;
done:
  closedir(dir);
  return ret;
}
#else
static int
copy_dir(file_task_t *task, const char *src, const char *dst) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(ensure_copy_dir(dst)) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while((entry = readdir(dir))) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(from, sizeof(from), src, entry->d_name) ||
       path_join(to, sizeof(to), dst, entry->d_name) ||
       copy_path(task, from, to)) {
      goto done;
    }
  }

  ret = 0;
done:
  closedir(dir);
  return ret;
}
#endif

static int
copy_path(file_task_t *task, const char *src, const char *dst) {
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return copy_dir(task, src, dst);
  }
  if(S_ISREG(st.st_mode)) {
    return copy_file(task, src, dst);
  }
  errno = ENOTSUP;
  return -1;
}

static int
move_path(file_task_t *task, const char *src, const char *dst) {
  struct stat src_st;
  struct stat dst_st;
  int dst_exists;
  int ret;

  if(lstat(src, &src_st)) {
    return -1;
  }
  dst_exists = !lstat(dst, &dst_st);
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(dst_exists && S_ISDIR(src_st.st_mode) && S_ISDIR(dst_st.st_mode)) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src);
    }
    return ret;
  }

  ret = rename(src, dst);
  if(ret && errno == EXDEV) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src);
    }
  }
  return ret;
}

static int
remove_path(file_task_t *task, const char *path) {
  struct stat st;

  task_update(task, TASK_RUNNING, path, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if(!dir) {
      return -1;
    }
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if(task_cancel_requested(task)) {
        closedir(dir);
        return -1;
      }
      if(path_join(child, sizeof(child), path, entry->d_name) ||
         remove_path(task, child)) {
        closedir(dir);
        return -1;
      }
    }
    closedir(dir);
    return rmdir(path);
  }
  return unlink(path);
}

/* Archive output is written only inside a unique, task-owned staging
 * directory.  This cleanup deliberately uses lstat, so an archive-supplied
 * symlink is unlinked rather than followed, even after cancellation. */
static int
remove_staging_tree(const char *path) {
  struct stat st;

  if(lstat(path, &st)) {
    return errno == ENOENT ? 0 : -1;
  }
  if(S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    int ret = 0;
    if(!dir) return -1;
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      if(path_join(child, sizeof(child), path, entry->d_name) ||
         remove_staging_tree(child)) {
        ret = -1;
        break;
      }
    }
    closedir(dir);
    if(!ret && rmdir(path)) ret = -1;
    return ret;
  }
  return unlink(path);
}

static int
resolve_destination(const char *src, const char *dst, char *out, size_t size) {
  struct stat st;

  if(!stat(dst, &st) && S_ISDIR(st.st_mode)) {
    return path_join(out, size, dst, path_basename(src));
  }
  if(strlen(dst) >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(out, dst);
  return 0;
}

static int
canonical_target_path(const char *path, char *out, size_t out_size) {
  char parent[PATH_MAX];
  char resolved_parent[PATH_MAX];

  if(realpath(path, out)) {
    return 0;
  }
  if(errno != ENOENT || path_dirname(path, parent, sizeof(parent)) ||
     !realpath(parent, resolved_parent)) {
    return -1;
  }
  return path_join(out, out_size, resolved_parent, path_basename(path));
}

static int
validate_task_target(const char *src, const char *target, int overwrite,
                     char *error, size_t error_size) {
  struct stat src_st;
  struct stat target_st;
  char canonical_src[PATH_MAX];
  char canonical_target[PATH_MAX];

  if(!strcmp(src, target)) {
    snprintf(error, error_size, "source and destination are the same");
    errno = EINVAL;
    return -1;
  }
  if(lstat(src, &src_st)) {
    snprintf(error, error_size, "source not found");
    return -1;
  }
  if(S_ISDIR(src_st.st_mode)) {
    size_t src_len;

    if(!realpath(src, canonical_src) ||
       canonical_target_path(target, canonical_target,
                             sizeof(canonical_target))) {
      snprintf(error, error_size, "cannot canonicalize source or destination");
      return -1;
    }
    src_len = strlen(canonical_src);
    while(src_len > 1 && canonical_src[src_len - 1] == '/') {
      src_len--;
    }
    if(!strncmp(canonical_src, canonical_target, src_len) &&
       (canonical_target[src_len] == 0 || canonical_target[src_len] == '/')) {
      snprintf(error, error_size, "destination is inside source directory");
      errno = EINVAL;
      return -1;
    }
  }
  if(lstat(target, &target_st)) {
    if(errno == ENOENT) {
      return 0;
    }
    snprintf(error, error_size, "cannot check destination");
    return -1;
  }
  if(src_st.st_dev == target_st.st_dev && src_st.st_ino == target_st.st_ino) {
    snprintf(error, error_size, "source and destination are the same");
    errno = EINVAL;
    return -1;
  }
  if(!overwrite) {
    snprintf(error, error_size, "destination exists");
    errno = EEXIST;
    return -1;
  }
  if(S_ISDIR(src_st.st_mode) != S_ISDIR(target_st.st_mode)) {
    snprintf(error, error_size, "remove conflicting file or directory first");
    errno = EEXIST;
    return -1;
  }
  return 0;
}

static int
move_requires_space_check(const char *src, const char *target) {
  struct stat src_st;
  struct stat target_st;
  struct stat dst_dir_st;
  char parent[PATH_MAX];

  if(lstat(src, &src_st)) {
    return -1;
  }
  if(!lstat(target, &target_st)) {
    if(S_ISDIR(src_st.st_mode) && S_ISDIR(target_st.st_mode)) {
      return 1;
    }
    return src_st.st_dev != target_st.st_dev;
  }
  if(path_dirname(target, parent, sizeof(parent)) || stat(parent, &dst_dir_st)) {
    return -1;
  }
  return src_st.st_dev != dst_dir_st.st_dev;
}

static int
check_task_targets_writable(file_task_t *task, char *error, size_t error_size,
                            char *code, size_t code_size, char *arg, size_t arg_size) {
  char **checked_dirs = NULL;
  size_t checked_dir_count = 0;
  size_t i;

  for(i = 0; i < task->src_count; i++) {
    char target[PATH_MAX];

    if(task_cancel_requested(task)) {
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
    if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
      snprintf(error, error_size, "target path is too long");
      snprintf(code, code_size, "target_path_too_long");
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
    if(check_target_writable(target, &checked_dirs, &checked_dir_count,
                             error, error_size, code, code_size, arg, arg_size)) {
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
  }
  free_paths(checked_dirs, checked_dir_count);
  return 0;
}

static int
task_target_path(file_task_t *task, const char *src, char *out, size_t size) {
  if(task->src_count > 1) {
    return path_join(out, size, task->dst, path_basename(src));
  }
  if(strlen(task->dst) >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(out, task->dst);
  return 0;
}

static int
request_target_path(const char *src, const char *dst, size_t src_count,
                    char *out, size_t size) {
  if(src_count > 1) {
    return path_join(out, size, dst, path_basename(src));
  }
  return resolve_destination(src, dst, out, size);
}

static enum MHD_Result
task_request_error(struct MHD_Connection *conn, file_task_t *task,
                   char **srcs, size_t src_count,
                   unsigned int status, const char *msg) {
  free_paths(srcs, src_count);
  free(task);
  return send_json_error(conn, status, msg);
}

typedef struct conversion_progress_ctx {
  file_task_t *task;
  unsigned long long total;
  unsigned long long last;
} conversion_progress_ctx_t;

static int
write_all_fd(int fd, const char *data, size_t size) {
  while(size) {
    ssize_t written = write(fd, data, size);

    if(written < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(!written) {
      errno = EIO;
      return -1;
    }
    data += (size_t)written;
    size -= (size_t)written;
  }
  return 0;
}

#define CONVERSION_JOURNAL_VERSION 3u
#define CONVERSION_JOURNAL_PREFIX "mkpfs-conversion-"
#define CONVERSION_JOURNAL_SUFFIX ".resume"

typedef struct conversion_journal {
  char magic[8];
  uint32_t version;
  uint32_t size;
  uint32_t compression_level;
  uint32_t workers;
  char source[PATH_MAX];
  char destination[PATH_MAX];
  char output_name[NAME_MAX];
  mkpfs_resume_state_t resume;
  uint64_t checksum;
} conversion_journal_t;

static uint64_t
conversion_hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;

  for(size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t
conversion_journal_checksum(const conversion_journal_t *journal) {
  return conversion_hash_bytes(UINT64_C(1469598103934665603), journal,
                               offsetof(conversion_journal_t, checksum));
}

static const char *
conversion_journal_directory(void) {
  const char *configured = getenv("WFM_RESUME_DIR");

  if(configured && configured[0] == '/') return configured;
#ifdef __linux__
  return "/tmp/mkpfs-resume";
#else
  return "/data/mkpfs-resume";
#endif
}

static int
ensure_conversion_journal_directory(void) {
  const char *directory = conversion_journal_directory();
  struct stat st;

  if(mkdir(directory, 0700) && errno != EEXIST) return -1;
  if(lstat(directory, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int
sync_conversion_journal_directory(void) {
  int fd;
  int error;

  fd = open(conversion_journal_directory(), O_RDONLY | O_DIRECTORY);
  if(fd < 0) return -1;
  if(fsync(fd)) {
    error = errno ? errno : EIO;
    close(fd);
    errno = error;
    return -1;
  }
  return close(fd) == 0 ? 0 : -1;
}

static int
conversion_journal_path(const char *output, char *path, size_t path_size) {
  uint64_t hash;

  if(!output || !path || ensure_conversion_journal_directory()) return -1;
  hash = conversion_hash_bytes(UINT64_C(1469598103934665603), output,
                               strlen(output) + 1);
  if(snprintf(path, path_size, "%s/" CONVERSION_JOURNAL_PREFIX "%016llx" \
              CONVERSION_JOURNAL_SUFFIX, conversion_journal_directory(),
              (unsigned long long)hash) >= (int)path_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int
conversion_journal_valid(const conversion_journal_t *journal) {
  if(memcmp(journal->magic, "MKPFSR1", 7) ||
     journal->version != CONVERSION_JOURNAL_VERSION ||
     journal->size != sizeof(*journal) ||
     journal->checksum != conversion_journal_checksum(journal) ||
     !memchr(journal->source, 0, sizeof(journal->source)) ||
     !memchr(journal->destination, 0, sizeof(journal->destination)) ||
     !memchr(journal->output_name, 0, sizeof(journal->output_name)) ||
     !memchr(journal->resume.inner_name, 0,
             sizeof(journal->resume.inner_name)) ||
     !memchr(journal->resume.exfat_path, 0,
             sizeof(journal->resume.exfat_path)) ||
     !memchr(journal->resume.pfs_path, 0, sizeof(journal->resume.pfs_path)) ||
     journal->compression_level > 9 || journal->workers > 8 ||
     journal->resume.phase < MKPFS_RESUME_EXFAT ||
     journal->resume.phase > MKPFS_RESUME_PUBLISH) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int
read_conversion_journal(const char *path, conversion_journal_t *journal) {
  struct stat st;
  int fd;
  ssize_t got;

  if(!path || !journal) {
    errno = EINVAL;
    return -1;
  }
  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if(fd < 0) return -1;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
     st.st_size != (off_t)sizeof(*journal)) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  got = read(fd, journal, sizeof(*journal));
  if(close(fd) || got != (ssize_t)sizeof(*journal)) {
    if(got >= 0) errno = EIO;
    return -1;
  }
  return conversion_journal_valid(journal);
}

static int
conversion_journal_stage_paths_valid(const char *journal_path,
                                     const conversion_journal_t *journal,
                                     char *note_path, size_t note_path_size) {
  char destination[PATH_MAX];
  char output[PATH_MAX];
  char expected_journal[PATH_MAX];
  char expected_exfat[PATH_MAX];
  char expected_pfs[PATH_MAX];
  uint64_t hash;

  if(!journal_path || !journal || !note_path ||
     mkpfs_normalize_path(journal->destination, destination,
                          sizeof(destination)) ||
     !relative_path_safe(journal->output_name) ||
     strchr(journal->output_name, '/') ||
     path_join(output, sizeof(output), destination, journal->output_name) ||
     conversion_journal_path(output, expected_journal, sizeof(expected_journal)) ||
     strcmp(journal_path, expected_journal)) {
    return 0;
  }
  hash = conversion_hash_bytes(UINT64_C(1469598103934665603), output,
                               strlen(output) + 1);
  if(snprintf(expected_exfat, sizeof(expected_exfat),
              "%s/.%s.mkpfs-%016llx.exfat.stage", destination,
              journal->output_name, (unsigned long long)hash) >=
              (int)sizeof(expected_exfat) ||
     snprintf(expected_pfs, sizeof(expected_pfs),
              "%s/.%s.mkpfs-%016llx.pfs.stage", destination,
              journal->output_name, (unsigned long long)hash) >=
              (int)sizeof(expected_pfs) ||
     snprintf(note_path, note_path_size, "%s/.%s.mkpfs-conversion.incomplete",
              destination, journal->output_name) >= (int)note_path_size ||
     strcmp(journal->resume.exfat_path, expected_exfat) ||
     strcmp(journal->resume.pfs_path, expected_pfs)) {
    return 0;
  }
  return 1;
}

static void
unlink_private_regular(const char *path, const char *label) {
  struct stat st;

  if(!path || !*path) return;
  if(lstat(path, &st) == 0) {
    if(S_ISREG(st.st_mode) && st.st_nlink == 1) {
      if(unlink(path)) fprintf(stderr, "%s cleanup failed: %s\n", label,
                               strerror(errno));
    } else {
      fprintf(stderr, "refusing unsafe %s cleanup: %s\n", label, path);
    }
  } else if(errno != ENOENT) {
    fprintf(stderr, "%s inspection failed: %s\n", label, strerror(errno));
  }
}

static void
cleanup_unrecoverable_conversion_journal(const char *journal_path,
                                         const conversion_journal_t *journal) {
  char note_path[PATH_MAX];

  if(!conversion_journal_stage_paths_valid(journal_path, journal, note_path,
                                           sizeof(note_path))) return;
  unlink_private_regular(journal->resume.exfat_path, "conversion exFAT stage");
  unlink_private_regular(journal->resume.pfs_path, "conversion PFS stage");
  unlink_private_regular(note_path, "conversion recovery note");
  unlink_private_regular(journal_path, "conversion journal");
}

static int
conversion_journal_has_final_output(const conversion_journal_t *journal) {
  char destination[PATH_MAX];
  char output[PATH_MAX];
  struct stat st;

  if(!journal || mkpfs_normalize_path(journal->destination, destination,
                                      sizeof(destination)) ||
     !relative_path_safe(journal->output_name) ||
     strchr(journal->output_name, '/') ||
     path_join(output, sizeof(output), destination, journal->output_name)) {
    return 0;
  }
  return lstat(output, &st) == 0 && S_ISREG(st.st_mode);
}

static int
write_conversion_journal(file_task_t *task) {
  conversion_journal_t journal;
  char temporary[PATH_MAX];
  int fd;

  if(!task || !task->conversion_journal[0]) {
    errno = EINVAL;
    return -1;
  }
  memset(&journal, 0, sizeof(journal));
  memcpy(journal.magic, "MKPFSR1", 7);
  journal.version = CONVERSION_JOURNAL_VERSION;
  journal.size = sizeof(journal);
  journal.compression_level = task->compression_level;
  journal.workers = task->conversion_workers;
  snprintf(journal.source, sizeof(journal.source), "%s", task->srcs[0]);
  {
    char parent[PATH_MAX];
    if(path_dirname(task->dst, parent, sizeof(parent))) return -1;
    snprintf(journal.destination, sizeof(journal.destination), "%s", parent);
  }
  snprintf(journal.output_name, sizeof(journal.output_name), "%s",
           task->conversion_name);
  journal.resume = task->conversion_resume;
  journal.checksum = conversion_journal_checksum(&journal);
  if(snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX",
              task->conversion_journal) >= (int)sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = mkstemp(temporary);
  if(fd < 0) return -1;
  if(fchmod(fd, 0600) || write_all_fd(fd, (const char *)&journal,
                                      sizeof(journal)) || fsync(fd)) {
    int error = errno ? errno : EIO;
    close(fd);
    unlink(temporary);
    errno = error;
    return -1;
  }
  if(close(fd)) {
    int error = errno ? errno : EIO;
    unlink(temporary);
    errno = error;
    return -1;
  }
  if(rename(temporary, task->conversion_journal)) {
    int error = errno ? errno : EIO;
    unlink(temporary);
    errno = error;
    return -1;
  }
  if(sync_conversion_journal_directory()) return -1;
  return 0;
}

static int
conversion_resume_checkpoint(const mkpfs_resume_state_t *resume, void *opaque) {
  file_task_t *task = opaque;

  if(!task || !resume) return -1;
  if(resume != &task->conversion_resume) return -1;
  return write_conversion_journal(task);
}

static void
remove_conversion_journal(file_task_t *task) {
  if(task->conversion_journal[0] && unlink(task->conversion_journal) &&
     errno != ENOENT) {
    fprintf(stderr, "conversion journal cleanup failed: %s\n", strerror(errno));
  }
  task->conversion_journal[0] = 0;
}

static int
prepare_conversion_resume(file_task_t *task) {
  char parent[PATH_MAX];
  const char *base;
  uint64_t hash;

  if(!task || !task->srcs || task->src_count != 1 ||
     path_dirname(task->dst, parent, sizeof(parent))) {
    errno = EINVAL;
    return -1;
  }
  if(task->conversion_journal[0]) return 0;
  if(conversion_journal_path(task->dst, task->conversion_journal,
                             sizeof(task->conversion_journal))) {
    return -1;
  }
  base = path_basename(task->dst);
  hash = conversion_hash_bytes(UINT64_C(1469598103934665603), task->dst,
                               strlen(task->dst) + 1);
  memset(&task->conversion_resume, 0, sizeof(task->conversion_resume));
  task->conversion_resume.phase = MKPFS_RESUME_EXFAT;
  if(!base[0] ||
     snprintf(task->conversion_resume.exfat_path,
              sizeof(task->conversion_resume.exfat_path),
              "%s/.%s.mkpfs-%016llx.exfat.stage", parent, base,
              (unsigned long long)hash) >=
              (int)sizeof(task->conversion_resume.exfat_path) ||
     snprintf(task->conversion_resume.pfs_path,
              sizeof(task->conversion_resume.pfs_path),
              "%s/.%s.mkpfs-%016llx.pfs.stage", parent, base,
              (unsigned long long)hash) >=
              (int)sizeof(task->conversion_resume.pfs_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if(!task->conversion_name[0]) {
    snprintf(task->conversion_name, sizeof(task->conversion_name), "%s", base);
  }
  return write_conversion_journal(task);
}

static void
remove_conversion_private_stages(file_task_t *task) {
  const char *paths[2];

  if(!task) return;
  paths[0] = task->conversion_resume.exfat_path;
  paths[1] = task->conversion_resume.pfs_path;
  for(size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    struct stat st;
    if(!paths[i][0]) continue;
    if(lstat(paths[i], &st) == 0) {
      if(S_ISREG(st.st_mode) && st.st_nlink == 1) {
        if(unlink(paths[i])) {
          fprintf(stderr, "conversion stage cleanup failed: %s\n", strerror(errno));
        }
      } else {
        fprintf(stderr, "refusing unsafe conversion stage cleanup: %s\n", paths[i]);
      }
    } else if(errno != ENOENT) {
      fprintf(stderr, "conversion stage inspect failed: %s\n", strerror(errno));
    }
  }
}

static int
conversion_stage_bytes(const file_task_t *task, uint64_t *bytes_out) {
  const char *paths[2];
  uint64_t total = 0;

  if(!task || !bytes_out) {
    errno = EINVAL;
    return -1;
  }
  paths[0] = task->conversion_resume.exfat_path;
  paths[1] = task->conversion_resume.pfs_path;
  for(size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    struct stat st;
    if(!paths[i][0]) continue;
    if(lstat(paths[i], &st)) {
      if(errno == ENOENT) continue;
      return -1;
    }
    if(!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
       UINT64_MAX - total < (uint64_t)st.st_size) {
      errno = EINVAL;
      return -1;
    }
    total += (uint64_t)st.st_size;
  }
  *bytes_out = total;
  return 0;
}

static int
create_conversion_recovery_note(file_task_t *task) {
  char parent[PATH_MAX];
  const char *base;
  char message[PATH_MAX * 2 + 512];
  int fd;
  int length;

  if(path_dirname(task->dst, parent, sizeof(parent))) return -1;
  base = path_basename(task->dst);
  if(!base[0] || snprintf(task->conversion_recovery_note,
                          sizeof(task->conversion_recovery_note),
                          "%s/.%s.mkpfs-conversion.incomplete",
                          parent, base) >=
                          (int)sizeof(task->conversion_recovery_note)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = open(task->conversion_recovery_note,
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if(fd < 0) {
    struct stat st;
    if(errno == EEXIST && lstat(task->conversion_recovery_note, &st) == 0 &&
       S_ISREG(st.st_mode) && st.st_nlink == 1) {
      return 0;
    }
    return -1;
  }
  length = snprintf(message, sizeof(message),
                    "An MkPFS folder conversion is in progress.\n"
                    "Source: %s\n"
                    "Final output: %s\n"
                    "Recovery journal: %s\n"
                    "If this note remains after the payload restarts, the "
                    "payload will validate the source and continue the last "
                    "durably checkpointed conversion stage automatically. "
                    "Do not use temporary files as final output.\n",
                    task->src, task->dst, task->conversion_journal);
  if(length < 0 || (size_t)length >= sizeof(message) ||
     write_all_fd(fd, message, (size_t)length) || fsync(fd)) {
    int error = errno ? errno : EIO;
    close(fd);
    unlink(task->conversion_recovery_note);
    task->conversion_recovery_note[0] = 0;
    errno = error;
    return -1;
  }
  if(close(fd)) {
    int error = errno ? errno : EIO;
    unlink(task->conversion_recovery_note);
    task->conversion_recovery_note[0] = 0;
    errno = error;
    return -1;
  }
  return 0;
}

static void
remove_conversion_recovery_note(file_task_t *task) {
  if(task->conversion_recovery_note[0]) {
    if(unlink(task->conversion_recovery_note) && errno != ENOENT) {
      fprintf(stderr, "conversion recovery note cleanup failed: %s\n",
              strerror(errno));
    }
    task->conversion_recovery_note[0] = 0;
  }
}

static int conversion_progress(uint64_t done, uint64_t phase_total, const char *phase, const char *current, void *opaque) {
  conversion_progress_ctx_t *ctx = opaque;
  unsigned long long start = 0;
  unsigned long long span = 0;
  unsigned long long target;
  if (!phase_total) phase_total = 1;
  if (phase && !strcmp(phase, "exfat")) {
    start = 0;
    span = ctx->total * 35u / 100u;
  } else if (phase && !strcmp(phase, "compress")) {
    start = ctx->total * 35u / 100u;
    span = ctx->total * 45u / 100u;
  } else if (phase && !strcmp(phase, "verify")) {
    start = ctx->total * 80u / 100u;
    span = ctx->total * 14u / 100u;
  } else if (phase && (!strcmp(phase, "assemble") || !strcmp(phase, "publish"))) {
    start = ctx->total * 94u / 100u;
    span = ctx->total - start;
  } else {
    start = ctx->total * 35u / 100u;
    span = ctx->total * 45u / 100u;
  }
  target = start + (unsigned long long)(((long double)done * (long double)span) /
                                         (long double)phase_total);
  if (target > ctx->total) target = ctx->total;
  if (target > ctx->last) {
    task_update(ctx->task, TASK_RUNNING, current ? current : phase, target - ctx->last, NULL);
    ctx->last = target;
  } else if (current) {
    task_update(ctx->task, TASK_RUNNING, current, 0, NULL);
  }
  return task_cancel_requested(ctx->task);
}

typedef struct archive_progress_ctx {
  file_task_t *task;
  unsigned int last_percent;
} archive_progress_ctx_t;

static void
archive_progress(unsigned int percent, const char *current, void *opaque) {
  archive_progress_ctx_t *ctx = opaque;
  unsigned int clamped = percent > 100 ? 100 : percent;
  if(current) task_update(ctx->task, TASK_RUNNING, current, 0, NULL);
  if(clamped > ctx->last_percent) {
    task_update(ctx->task, TASK_RUNNING, NULL, clamped - ctx->last_percent, NULL);
    ctx->last_percent = clamped;
  }
}

static void
task_worker_release(file_task_t *task) {
  pthread_mutex_lock(&g_tasks_lock);
  task->worker_active = 0;
  pthread_mutex_unlock(&g_tasks_lock);
}

#define TASK_WORKER_RETURN() do { \
  task_worker_release(task); \
  return NULL; \
} while(0)

void *
task_worker(void *arg) {
  file_task_t *task = arg;
  unsigned long long total = 0;
  unsigned long long required = 0;
  int ret = -1;

  pthread_mutex_lock(&g_tasks_lock);
  task->worker_active = 1;
  pthread_mutex_unlock(&g_tasks_lock);
  task_update(task, TASK_RUNNING, "preparing", 0, NULL);

  if(task_cancel_requested(task)) {
    if(task->op == TASK_CONVERT) {
      remove_conversion_private_stages(task);
      remove_conversion_journal(task);
      remove_conversion_recovery_note(task);
    }
    task_update(task, TASK_CANCELED, task->src, 0, "canceled");
    TASK_WORKER_RETURN();
  }
  if(task->op == TASK_CONVERT) {
    mkpfs_scan_result_t scan;
    mkpfs_native_options_t options = {0};
    conversion_progress_ctx_t progress = {.task = task};
    uint64_t workspace = 0;
    uint64_t existing_stage_bytes = 0;
    uint64_t durable_exfat_size = 0;
    unsigned long long available = 0;
    char output_dir[PATH_MAX];
    char *output_name;
    snprintf(output_dir, sizeof(output_dir), "%s", task->dst);
    output_name = strrchr(output_dir, '/');
    if (output_name) {
      *output_name++ = 0;
      if(!output_dir[0]) snprintf(output_dir, sizeof(output_dir), "/");
    } else {
      output_name = output_dir;
      snprintf(output_dir, sizeof(output_dir), ".");
    }
    if(task->conversion_recovered &&
       task->conversion_resume.phase >= MKPFS_RESUME_PACK) {
      struct stat stage_st;
      if(lstat(task->conversion_resume.exfat_path, &stage_st) ||
         !S_ISREG(stage_st.st_mode) || stage_st.st_nlink != 1 ||
         stage_st.st_size <= 0) {
        ret = ESTALE;
        task_set_error_code(task, "resume_stage", task->dst);
      } else {
        durable_exfat_size = (uint64_t)stage_st.st_size;
        ret = mkpfs_estimate_workspace_from_exfat(durable_exfat_size, &workspace);
      }
    } else if (mkpfs_scan_folder(task->srcs[0], &scan)) {
      ret = errno ? errno : EIO;
    } else if((ret = mkpfs_estimate_conversion_workspace(&scan, &workspace)) != 0) {
      task_set_error_code(task, "conversion_size", task->srcs[0]);
    }
    if(!ret && target_available_space(output_dir, &available)) {
      ret = errno ? errno : EIO;
      task_set_error_code(task, "space_check_failed", output_dir);
    }
    if(!ret && task->conversion_recovered &&
       conversion_stage_bytes(task, &existing_stage_bytes)) {
      ret = errno ? errno : EIO;
      task_set_error_code(task, "resume_stage", task->dst);
    }
    if(!ret && available < workspace &&
       (UINT64_MAX - available < existing_stage_bytes ||
        available + existing_stage_bytes < workspace)) {
      ret = ENOSPC;
      task_set_error_code(task, "no_space", output_dir);
    }
    if(!ret) {
      if(durable_exfat_size) {
        progress.total = durable_exfat_size > (ULLONG_MAX - 1u) / 2u ?
                         ULLONG_MAX : durable_exfat_size * 2u + 1u;
      } else if(scan.total_bytes > (ULLONG_MAX - 1u) / 2u) {
        progress.total = ULLONG_MAX;
      } else {
        progress.total = scan.total_bytes ? scan.total_bytes * 2u + 1u : 1u;
      }
      task_set_total(task, progress.total);
      options.compression_level = task->compression_level <= 9 ? task->compression_level : 7;
      options.workers = task->conversion_workers;
      options.compression = 1; options.verify = 1; options.verify_structure = 1;
      {
        /* Once exFAT is durable, packing and verification read only that
         * immutable private stage.  Re-scanning a large source tree here
         * adds hours without improving recovery safety.  An EXFAT-stage
         * restart intentionally rebuilds the snapshot from the current
         * source and each file read still checks inode, device, and size. */
        if(prepare_conversion_resume(task)) {
        ret = errno ? errno : EIO;
        task_set_error_code(task, "resume_journal", task->dst);
        } else if(create_conversion_recovery_note(task)) {
        ret = errno ? errno : EIO;
        task_set_error_code(task, "resume_note", task->dst);
        } else {
        task_update(task, TASK_RUNNING,
                    task->conversion_recovered ? "resuming conversion" :
                    "native conversion", 0, NULL);
        ret = mkpfs_convert_folder_resumable(
          task->srcs[0], output_dir, output_name, &options,
          &task->cancel_requested, conversion_progress, &progress,
          &task->conversion_resume, conversion_resume_checkpoint, task);
        }
      }
    }
  }
  if(task->op == TASK_EXTRACT) {
    char parent[PATH_MAX];
    char staging[PATH_MAX];
    char archive_error[160] = {0};
    archive_progress_ctx_t progress = {.task = task, .last_percent = 0};
    struct stat destination_st;

    if(path_dirname(task->dst, parent, sizeof(parent)) ||
       lstat(parent, &destination_st) || !S_ISDIR(destination_st.st_mode) ||
       snprintf(staging, sizeof(staging), "%s/.mkpfs-extract-%lu.tmp", parent,
                task->id) >= (int)sizeof(staging)) {
      ret = EINVAL;
    } else if(mkdir(staging, 0700)) {
      ret = errno;
    } else {
      task_set_total(task, 100);
      task_update(task, TASK_RUNNING, "extracting archive", 0, NULL);
      ret = archive_extract_run(task->srcs[0], staging, task->archive_password,
                                task->conversion_workers,
                                (volatile int *)&task->cancel_requested,
                                archive_progress, &progress, archive_error,
                                sizeof(archive_error));
      if(ret == 0 && !task_cancel_requested(task) && rename(staging, task->dst)) {
        ret = errno;
      }
      if(ret != 0 || task_cancel_requested(task)) {
        int cleanup_errno;
        if(remove_staging_tree(staging)) {
          cleanup_errno = errno;
          if(!archive_error[0]) snprintf(archive_error, sizeof(archive_error),
                                         "staging cleanup failed: %s",
                                         strerror(cleanup_errno));
        }
      }
      if(ret != 0 && archive_error[0]) {
        task_set_error_code(task, "archive_extract_failed", task->srcs[0]);
        task_update(task, TASK_RUNNING, task->srcs[0], 0, archive_error);
      }
    }
    if(task_cancel_requested(task) && ret == 0) ret = ECANCELED;
    if(ret == 255) {
      errno = ECANCELED;
      ret = ECANCELED;
    } else if(ret > 0 && ret < 256) {
      errno = EIO;
      ret = EIO;
    }
  }
  if(task->op == TASK_URL_DOWNLOAD) {
    ret = url_download_task_run(task);
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    char error[160] = {0};
    char code[64] = {0};
    char arg[PATH_MAX + 96] = {0};

    task_update(task, TASK_RUNNING, "checking target permissions", 0, NULL);
    if(check_task_targets_writable(task, error, sizeof(error),
                                   code, sizeof(code), arg, sizeof(arg))) {
      if(errno == ECANCELED || task_cancel_requested(task)) {
        task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                    0, "canceled");
      } else {
        task_set_error_code(task, code, arg);
        task_update(task, TASK_FAILED, task->current[0] ? task->current : task->dst,
                    0, error[0] ? error : strerror(errno));
      }
      TASK_WORKER_RETURN();
    }
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    size_t i;
    for(i = 0; i < task->src_count; i++) {
      unsigned long long before = total;
      int needs_space = task->op == TASK_COPY;
      char target[PATH_MAX];

      if(task->op == TASK_COPY || task->op == TASK_MOVE) {
        if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
          TASK_WORKER_RETURN();
        }
      }
      if(task->op == TASK_MOVE) {
        needs_space = move_requires_space_check(task->srcs[i], target);
        if(needs_space < 0) {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
          TASK_WORKER_RETURN();
        }
        if(!needs_space) {
          continue;
        }
      }
      if(count_path_bytes(task, task->srcs[i], task->srcs[i], &total,
                          &task->file_count, &task->dir_count)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, task->srcs[i], 0, "canceled");
        } else {
          if(errno == ENAMETOOLONG) {
            task_set_error_code(task, "path_too_long", NULL);
          }
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
        }
        TASK_WORKER_RETURN();
      }
      if(needs_space) {
        required += total - before;
      }
    }
    task_set_total(task, total);

    if(task->op == TASK_COPY || task->op == TASK_MOVE) {
      char error[128] = {0};
      char code[64] = {0};
      char arg[PATH_MAX] = {0};

      if(check_target_space(task->dst, required, error, sizeof(error),
                            code, sizeof(code), arg, sizeof(arg))) {
        task_set_error_code(task, code, arg);
        task_update(task, TASK_FAILED, task->dst, 0, error[0] ? error : strerror(errno));
        TASK_WORKER_RETURN();
      }
    }
  }

  if(task->op == TASK_COPY) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      char target[PATH_MAX];
      if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
        ret = -1;
        break;
      }
      ret = copy_path(task, task->srcs[i], target);
    }
  } else if(task->op == TASK_MOVE) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      char target[PATH_MAX];
      if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
        ret = -1;
        break;
      }
      ret = move_path(task, task->srcs[i], target);
      if(!ret && task->src_count == 1) {
        task_update(task, TASK_RUNNING, target, total, NULL);
      }
    }
  } else if(task->op == TASK_DELETE) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      ret = remove_path(task, task->srcs[i]);
    }
  } else if(task->op == TASK_CHMOD) {
    unsigned long long ignored_bytes = 0;
    size_t i;

    ret = 0;
    if(task->recursive) {
      for(i = 0; i < task->src_count; i++) {
        size_t files_before = task->file_count;
        size_t dirs_before = task->dir_count;

        if(count_task_path_bytes(task, task->srcs[i], task->srcs[i],
                                 &ignored_bytes, &task->file_count,
                                 &task->dir_count)) {
          ret = -1;
          break;
        }
        if(files_before == task->file_count && dirs_before == task->dir_count) {
          task->file_count++;
        }
      }
      total = task->file_count + task->dir_count;
    } else {
      total = task->src_count;
    }
    if(!ret) {
      task_set_total(task, total);
      for(i = 0; i < task->src_count && !ret; i++) {
        ret = chmod_task_path(task, task->srcs[i], task->chmod_mode,
                              task->recursive);
      }
    }
  }

  if(ret) {
    errno = ret;
    if(errno == ECANCELED || task_cancel_requested(task)) {
      if(task->op == TASK_CONVERT) {
        remove_conversion_private_stages(task);
        remove_conversion_journal(task);
        remove_conversion_recovery_note(task);
      }
      task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                  0, "canceled");
    } else {
      if(task->op == TASK_CONVERT &&
         (errno == ESTALE || (task->conversion_recovered && errno == EINVAL))) {
        /* Do not retain a snapshot whose source changed, or retry a recovered
         * checkpoint whose private staging structure cannot be validated. */
        remove_conversion_private_stages(task);
        remove_conversion_journal(task);
        remove_conversion_recovery_note(task);
        task_set_error_code(task, errno == ESTALE ? "conversion_source_changed" :
                                                "resume_checkpoint_invalid",
                            task->src);
      }
      if(task->op == TASK_CHMOD) {
        task_set_error_code(task, "chmod_failed",
                            task->current[0] ? task->current : task->src);
      }
      task_update(task, TASK_FAILED, task->current[0] ? task->current : task->src,
                  0, task->error[0] ? task->error : strerror(errno));
    }
  } else {
    time_t completed_at = time(NULL);
    if(task->op == TASK_CONVERT) {
      remove_conversion_private_stages(task);
      remove_conversion_journal(task);
      remove_conversion_recovery_note(task);
    }
    pthread_mutex_lock(&g_tasks_lock);
    if (task->op == TASK_CONVERT || task->op == TASK_EXTRACT ||
        task->op == TASK_URL_DOWNLOAD)
      snprintf(task->current, sizeof(task->current), "%s", task->dst);
    task->state = TASK_DONE;
    if(task->total) {
      task->done = task->total;
    }
    task->updated_at = completed_at;
    record_task_completion_locked(task, completed_at);
    pthread_mutex_unlock(&g_tasks_lock);
  }

  TASK_WORKER_RETURN();
}
#undef TASK_WORKER_RETURN

static file_task_t *
task_from_conversion_journal(const char *path, const conversion_journal_t *journal) {
  file_task_t *task;
  char output[PATH_MAX];
  char normalized_source[PATH_MAX];
  char normalized_destination[PATH_MAX];
  char expected_journal[PATH_MAX];
  struct stat source_st;
  struct stat destination_st;
  struct stat output_st;
  int source_required;

  if(!path || !journal || mkpfs_normalize_path(journal->source, normalized_source,
                                               sizeof(normalized_source)) ||
     mkpfs_normalize_path(journal->destination, normalized_destination,
                          sizeof(normalized_destination)) ||
     !relative_path_safe(journal->output_name) ||
     strchr(journal->output_name, '/') ||
     path_join(output, sizeof(output), normalized_destination,
               journal->output_name) ||
     conversion_journal_path(output, expected_journal, sizeof(expected_journal)) ||
     strcmp(path, expected_journal) || lstat(normalized_destination, &destination_st) ||
     !S_ISDIR(destination_st.st_mode) ||
     lstat(output, &output_st) == 0 || errno != ENOENT) {
    return NULL;
  }
  source_required = journal->resume.phase == MKPFS_RESUME_EXFAT;
  if((source_required && (lstat(normalized_source, &source_st) ||
                          !S_ISDIR(source_st.st_mode)))) {
    return NULL;
  }
  task = calloc(1, sizeof(*task));
  if(!task) return NULL;
  task->srcs = calloc(1, sizeof(*task->srcs));
  if(!task->srcs || !(task->srcs[0] = strdup(normalized_source))) {
    free_task(task);
    return NULL;
  }
  atomic_init(&task->cancel_requested, 0);
  task->src_count = 1;
  task->op = TASK_CONVERT;
  task->state = TASK_QUEUED;
  task->compression_level = journal->compression_level;
  task->conversion_workers = journal->workers;
  task->conversion_resume = journal->resume;
  task->conversion_recovered = 1;
  snprintf(task->src, sizeof(task->src), "%s", normalized_source);
  snprintf(task->dst, sizeof(task->dst), "%s", output);
  snprintf(task->conversion_name, sizeof(task->conversion_name), "%s",
           journal->output_name);
  snprintf(task->conversion_journal, sizeof(task->conversion_journal), "%s", path);
  task->created_at = task->updated_at = time(NULL);
  return task;
}

int
filemgr_resume_interrupted_conversions(void) {
  DIR *directory;
  struct dirent *entry;
  file_task_t *task = NULL;
  char selected[PATH_MAX] = {0};

  if(ensure_conversion_journal_directory()) {
    fprintf(stderr, "conversion recovery directory unavailable: %s\n",
            strerror(errno));
    return -1;
  }
  directory = opendir(conversion_journal_directory());
  if(!directory) return -1;
  while((entry = readdir(directory)) != NULL) {
    char path[PATH_MAX];
    conversion_journal_t journal;
    size_t name_length = strlen(entry->d_name);

    if(strncmp(entry->d_name, CONVERSION_JOURNAL_PREFIX,
               strlen(CONVERSION_JOURNAL_PREFIX)) ||
       name_length <= strlen(CONVERSION_JOURNAL_PREFIX) +
                      strlen(CONVERSION_JOURNAL_SUFFIX) ||
       strcmp(entry->d_name + name_length - strlen(CONVERSION_JOURNAL_SUFFIX),
              CONVERSION_JOURNAL_SUFFIX) ||
       snprintf(path, sizeof(path), "%s/%s", conversion_journal_directory(),
                entry->d_name) >= (int)sizeof(path) ||
       read_conversion_journal(path, &journal)) {
      continue;
    }
    task = task_from_conversion_journal(path, &journal);
    if(!task && conversion_journal_has_final_output(&journal)) {
      /* A process can die after atomic publication and before it removes the
       * journal.  The completed output wins; reclaim only matching private
       * stages whose deterministic paths were validated from the journal. */
      cleanup_unrecoverable_conversion_journal(path, &journal);
      continue;
    }
    if(task) {
      snprintf(selected, sizeof(selected), "%s", path);
      break;
    }
  }
  closedir(directory);
  if(!task) return 0;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);
    return 0;
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);
  if(pthread_create(&task->thread, NULL, task_worker, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "recovery worker creation failed");
    return -1;
  }
  pthread_detach(task->thread);
  fprintf(stderr, "resuming interrupted conversion from %s\n", selected);
  return 1;
}

static enum MHD_Result
create_task_response(struct MHD_Connection *conn, task_op_t op,
                     char **srcs, size_t src_count, const char *dst,
                     int overwrite, unsigned int chmod_mode, int recursive) {
  file_task_t *task = calloc(1, sizeof(file_task_t));
  strbuf_t b = {0};
  struct stat st;
  size_t i;

  if(!task) {
    free_paths(srcs, src_count);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  atomic_init(&task->cancel_requested, 0);
  if(!src_count) {
    return task_request_error(conn, task, srcs, src_count,
                              MHD_HTTP_BAD_REQUEST, "no source paths");
  }
  if(dst && src_count > 1) {
    if(stat(dst, &st) || !S_ISDIR(st.st_mode)) {
      return task_request_error(conn, task, srcs, src_count,
                                MHD_HTTP_BAD_REQUEST,
                                "destination must be a directory for multiple items");
    }
  }
  if(dst) {
    for(i = 0; i < src_count; i++) {
      char target[PATH_MAX];
      char error[128] = {0};
      size_t j;
      if(request_target_path(srcs[i], dst, src_count, target, sizeof(target))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_BAD_REQUEST, NULL);
      }
      for(j = 0; j < i; j++) {
        char previous_target[PATH_MAX];
        if(request_target_path(srcs[j], dst, src_count, previous_target,
                               sizeof(previous_target))) {
          return task_request_error(conn, task, srcs, src_count,
                                    MHD_HTTP_BAD_REQUEST, NULL);
        }
        if(!strcmp(target, previous_target)) {
          return task_request_error(conn, task, srcs, src_count,
                                    MHD_HTTP_CONFLICT, "destination exists");
        }
      }
      if(validate_task_target(srcs[i], target, overwrite, error, sizeof(error))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_CONFLICT, error[0] ? error : NULL);
      }
    }
  }

  task->op = op;
  task->state = TASK_QUEUED;
  task->chmod_mode = chmod_mode;
  task->recursive = recursive;
  task->srcs = srcs;
  task->src_count = src_count;
  snprintf(task->src, sizeof(task->src), "%s%s",
           srcs[0], src_count > 1 ? " ..." : "");
  if(dst) {
    if(src_count == 1) {
      char target[PATH_MAX];
      if(request_target_path(srcs[0], dst, src_count, target, sizeof(target))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_BAD_REQUEST, NULL);
      }
      snprintf(task->dst, sizeof(task->dst), "%s", target);
    } else {
      snprintf(task->dst, sizeof(task->dst), "%s", dst);
    }
  }
  task->created_at = time(NULL);
  task->updated_at = task->created_at;

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    return task_request_error(conn, task, srcs, src_count,
                              MHD_HTTP_CONFLICT, "another task is running");
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  if(pthread_create(&task->thread, NULL, task_worker, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }

  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_convert(struct MHD_Connection *conn) {
  char *source = absolute_path_value(query_value(conn, "source"));
  char *destination = absolute_path_value(query_value(conn, "destination"));
  char *name = fs_path_value(query_value(conn, "name"));
  char *profile = query_value(conn, "profile");
  char *workers_param = query_value(conn, "workers");
  struct stat source_st, destination_st;
  mkpfs_scan_result_t scan;
  unsigned long level = profile ? strtoul(profile, NULL, 10) : 7;
  unsigned long workers = 0;
  unsigned long long available = 0;
  if (workers_param && strcasecmp(workers_param, "auto")) workers = strtoul(workers_param, NULL, 10);
  file_task_t *task = NULL;
  strbuf_t b = {0};
  char **srcs = NULL;
  char output[PATH_MAX];
  int rc = MHD_HTTP_BAD_REQUEST;

  if (!source || !destination || !name || !relative_path_safe(name) || level > 9 || workers > 8 || (workers_param && strcasecmp(workers_param, "auto") && workers == 0) || stat(source, &source_st) || !S_ISDIR(source_st.st_mode) || stat(destination, &destination_st) || !S_ISDIR(destination_st.st_mode)) {
    rc = MHD_HTTP_BAD_REQUEST; goto convert_error;
  }
  if (mkpfs_scan_folder(source, &scan)) { rc = MHD_HTTP_BAD_REQUEST; goto convert_error; }
  uint64_t required_workspace = 0;
  if (mkpfs_estimate_conversion_workspace(&scan, &required_workspace) ||
      target_available_space(destination, &available) ||
      available < required_workspace) {
    rc = MHD_HTTP_INSUFFICIENT_STORAGE; goto convert_error;
  }
  if (snprintf(output, sizeof(output), "%s/%s", destination, name) >= (int)sizeof(output)) { rc = MHD_HTTP_BAD_REQUEST; goto convert_error; }
  srcs = calloc(1, sizeof(*srcs)); task = calloc(1, sizeof(*task));
  if (!srcs || !task) { rc = MHD_HTTP_INTERNAL_SERVER_ERROR; goto convert_error; }
  atomic_init(&task->cancel_requested, 0);
  srcs[0] = source; source = NULL; task->srcs = srcs; srcs = NULL; task->src_count = 1;
  task->op = TASK_CONVERT; task->state = TASK_QUEUED; task->compression_level = (unsigned int)level; task->conversion_workers = (unsigned int)workers;
  snprintf(task->src, sizeof(task->src), "%s", task->srcs[0]); snprintf(task->dst, sizeof(task->dst), "%s", output); snprintf(task->conversion_name, sizeof(task->conversion_name), "%s", name);
  task->created_at = task->updated_at = time(NULL);
  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if (has_active_task_locked()) { pthread_mutex_unlock(&g_tasks_lock); rc = MHD_HTTP_CONFLICT; goto convert_error; }
  task->id = g_next_task_id++; task->next = g_tasks; g_tasks = task; pthread_mutex_unlock(&g_tasks_lock);
  if (pthread_create(&task->thread, NULL, task_worker, task)) { task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed"); } else pthread_detach(task->thread);
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  free(destination); free(name); free(profile); free(workers_param); return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
convert_error:
  free(source); free(destination); free(name); free(profile); free(workers_param); free(srcs); if (task) { free(task->srcs); free(task); }
  return send_json_error(conn, rc,
                         rc == MHD_HTTP_INSUFFICIENT_STORAGE ?
                         "insufficient safe working space for temporary image and final output" :
                         "invalid conversion request");
}

static enum MHD_Result
api_extract(struct MHD_Connection *conn) {
  char *source = absolute_path_value(query_value(conn, "source"));
  char *destination = absolute_path_value(query_value(conn, "destination"));
  char *name = fs_path_value(query_value(conn, "name"));
  char *password = query_value(conn, "password");
  char *workers_param = query_value(conn, "workers");
  struct stat source_st, destination_st, output_st;
  unsigned long workers = 0;
  unsigned long long available = 0;
  char output[PATH_MAX];
  file_task_t *task = NULL;
  char **srcs = NULL;
  strbuf_t b = {0};
  int rc = MHD_HTTP_BAD_REQUEST;

  if(workers_param && strcasecmp(workers_param, "auto")) workers = strtoul(workers_param, NULL, 10);
  if(!source || !destination || !name || !relative_path_safe(name) || strchr(name, '/') ||
     (password && strlen(password) >= sizeof(task->archive_password)) || workers > 8 ||
     (workers_param && strcasecmp(workers_param, "auto") && workers == 0) ||
     lstat(source, &source_st) || !S_ISREG(source_st.st_mode) ||
     lstat(destination, &destination_st) || !S_ISDIR(destination_st.st_mode) ||
     path_join(output, sizeof(output), destination, name)) {
    goto extract_error;
  }
  if(lstat(output, &output_st) == 0 || errno != ENOENT) {
    rc = MHD_HTTP_CONFLICT;
    goto extract_error;
  }
  /* An archive can expand beyond its compressed size.  Preserve a generous
   * reserve so an extraction cannot start on an almost-full destination. */
  if(target_available_space(destination, &available) || available < 64ULL * 1024ULL * 1024ULL) {
    rc = MHD_HTTP_INSUFFICIENT_STORAGE;
    goto extract_error;
  }
  srcs = calloc(1, sizeof(*srcs));
  task = calloc(1, sizeof(*task));
  if(!srcs || !task) {
    rc = MHD_HTTP_INTERNAL_SERVER_ERROR;
    goto extract_error;
  }
  atomic_init(&task->cancel_requested, 0);
  srcs[0] = source;
  source = NULL;
  task->srcs = srcs;
  srcs = NULL;
  task->src_count = 1;
  task->op = TASK_EXTRACT;
  task->state = TASK_QUEUED;
  task->conversion_workers = (unsigned int)workers;
  snprintf(task->src, sizeof(task->src), "%s", task->srcs[0]);
  snprintf(task->dst, sizeof(task->dst), "%s", output);
  if(password) snprintf(task->archive_password, sizeof(task->archive_password), "%s", password);
  task->created_at = task->updated_at = time(NULL);
  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    rc = MHD_HTTP_CONFLICT;
    goto extract_error;
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);
  if(pthread_create(&task->thread, NULL, task_worker, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  free(destination); free(name); free(password); free(workers_param);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");

extract_error:
  free(source); free(destination); free(name); free(password); free(workers_param);
  free(srcs);
  if(task) {
    free(task->srcs);
    free(task);
  }
  return send_json_error(conn, rc,
    rc == MHD_HTTP_INSUFFICIENT_STORAGE ? "insufficient storage" :
    rc == MHD_HTTP_CONFLICT ? "destination exists or another task is running" :
    "invalid extraction request");
}

static enum MHD_Result
api_tasks(struct MHD_Connection *conn) {
  strbuf_t b = {0};
  file_task_t *task;
  time_t now = time(NULL);
  int first = 1;

  strbuf_append(&b, "{\"ok\":true,\"tasks\":[");
  pthread_mutex_lock(&g_tasks_lock);
  for(task = g_tasks; task; task = task->next) {
    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_printf(&b, "{\"id\":%lu,\"op\":\"%s\",\"state\":\"%s\",",
                  task->id, task_op_name(task->op), task_state_name(task->state));
    strbuf_append(&b, "\"src\":");
    json_escape(&b, task->src);
    strbuf_append(&b, ",\"dst\":");
    json_escape(&b, task->dst);
    strbuf_append(&b, ",\"current\":");
    json_escape(&b, task->current);
    strbuf_append(&b, ",\"error\":");
    json_escape(&b, task->error);
    strbuf_append(&b, ",\"error_code\":");
    json_escape(&b, task->error_code);
    strbuf_append(&b, ",\"error_arg\":");
    json_escape(&b, task->error_arg);
    strbuf_printf(&b, ",\"src_count\":%zu,\"completed_count\":%zu,\"total\":%llu,\"done\":%llu,\"speed\":%llu,\"eta\":%llu,\"cancel_requested\":%s,\"created_at\":%lld,\"elapsed\":%lld,\"total_elapsed\":%lld,\"updated_at\":%lld}",
                  task->src_count, task->upload_completed,
                  task->total, task->done, task->speed, task->eta,
                  atomic_load_explicit(&task->cancel_requested, memory_order_acquire) ? "true" : "false",
                  (long long)task->created_at,
                  task->transfer_started_at ? (long long)(now - task->transfer_started_at) : 0LL,
                  task->created_at ? (long long)(now - task->created_at) : 0LL,
                  (long long)task->updated_at);
    if(!task_is_active(task)) task->reported = 1;
  }
  strbuf_printf(&b, "],\"now\":%lld,\"completion\":", (long long)now);
  if(g_last_completion.id) {
    strbuf_printf(&b, "{\"id\":%lu,\"op\":\"%s\",\"src\":",
                  g_last_completion.id, task_op_name(g_last_completion.op));
    json_escape(&b, g_last_completion.src);
    strbuf_printf(&b, ",\"src_count\":%zu,\"elapsed\":%lld,\"total\":%llu,\"file_count\":%zu}",
                  g_last_completion.src_count,
                  (long long)g_last_completion.elapsed,
                  g_last_completion.total, g_last_completion.file_count);
  } else {
    strbuf_append(&b, "null");
  }
  remove_finished_tasks_locked();
  pthread_mutex_unlock(&g_tasks_lock);
  strbuf_append(&b, "}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_cancel(struct MHD_Connection *conn) {
  char *idstr = query_value(conn, "id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task;
  int found = 0;

  free(idstr);
  pthread_mutex_lock(&g_tasks_lock);
  for(task = g_tasks; task; task = task->next) {
    if(task->id == id && task->op != TASK_PKG_INSTALL && task_is_active(task)) {
      atomic_store_explicit(&task->cancel_requested, 1, memory_order_release);
      if(task->op == TASK_DOWNLOAD && task->state == TASK_QUEUED) {
        task->state = TASK_CANCELED;
        snprintf(task->error, sizeof(task->error), "canceled");
      }
      task->updated_at = time(NULL);
      found = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_tasks_lock);

  return found ? send_json_ok(conn) :
                 send_json_error(conn, MHD_HTTP_NOT_FOUND, "active task not found");
}

void
filemgr_cancel_and_wait_for_tasks(void) {
  for(;;) {
    file_task_t *task;
    int active = 0;

    pthread_mutex_lock(&g_tasks_lock);
    for(task = g_tasks; task; task = task->next) {
      if(task_is_active(task)) {
        atomic_store_explicit(&task->cancel_requested, 1, memory_order_release);
        if(task->op == TASK_PKG_INSTALL && task->state == TASK_QUEUED) {
          task->state = TASK_CANCELED;
          snprintf(task->error, sizeof(task->error), "canceled");
        } else {
          active = 1;
        }
      }
    }
    pthread_mutex_unlock(&g_tasks_lock);
    if(!active) return;
    usleep(10000);
  }
}

static void *
stop_websrv_later(void *arg) {
  (void)arg;
  usleep(250000);
#ifndef __linux__
  (void)navigate_to_home();
  /* Keep this process alive while ShellUI handles the asynchronous URI. */
  usleep(1000000);
#endif
  filemgr_cancel_and_wait_for_tasks();
  websrv_stop();
  return NULL;
}

static enum MHD_Result
api_exit(struct MHD_Connection *conn) {
  pthread_t thread;

  if(!pthread_create(&thread, NULL, stop_websrv_later, NULL)) {
    pthread_detach(thread);
  }
  return send_json_ok(conn);
}

static enum MHD_Result
api_copy(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char *dst = absolute_path_value(body_form_value(body, body_size, "dst"));
  char *overwrite = body_form_value(body, body_size, "overwrite");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || !dst || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(dst); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  if(count == 1) {
    char target[PATH_MAX];
    if(request_target_path(paths[0], dst, count, target, sizeof(target)) ||
       !strcmp(paths[0], target)) {
      free_paths(paths, count); free(paths_raw); free(dst); free(overwrite);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "source and destination are the same");
    }
  }
  ret = create_task_response(conn, TASK_COPY, paths, count, dst,
                             overwrite && !strcmp(overwrite, "1"), 0, 0);
  free(paths_raw); free(dst); free(overwrite);
  return ret;
}

static enum MHD_Result
api_move(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char *dst = absolute_path_value(body_form_value(body, body_size, "dst"));
  char *overwrite = body_form_value(body, body_size, "overwrite");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || !dst || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(dst); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  ret = create_task_response(conn, TASK_MOVE, paths, count, dst,
                             overwrite && !strcmp(overwrite, "1"), 0, 0);
  free(paths_raw); free(dst); free(overwrite);
  return ret;
}

static enum MHD_Result
api_delete(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  for(size_t i = 0; i < count; i++) {
    if(!strcmp(paths[i], "/")) {
      free_paths(paths, count); free(paths_raw);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
    }
  }
  ret = create_task_response(conn, TASK_DELETE, paths, count, NULL, 0, 0, 0);
  free(paths_raw);
  return ret;
}

static enum MHD_Result
api_rename(struct MHD_Connection *conn) {
  char *path = absolute_path_value(query_value(conn, "path"));
  char *name = fs_path_value(query_value(conn, "name"));
  char parent[PATH_MAX];
  char target[PATH_MAX];
  int ret;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!path || !name || path_dirname(path, parent, sizeof(parent)) ||
     path_join(target, sizeof(target), parent, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }

  ret = rename(path, target);
  free(path); free(name);
  return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL)
             : send_json_ok(conn);
}

static enum MHD_Result
api_mkdir(struct MHD_Connection *conn) {
  char *path = absolute_path_value(query_value(conn, "path"));
  char *name = fs_path_value(query_value(conn, "name"));
  char target[PATH_MAX];
  int ret;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!path || !name || path_join(target, sizeof(target), path, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  ret = mkdir(target, 0777);
  if(!ret) {
    ret = chmod_path_0777(target);
  }
  free(path); free(name);
  return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL)
             : send_json_ok(conn);
}

static int
parse_permission_mode(const char *text, unsigned int *mode) {
  size_t i;

  if(!text || strlen(text) != 4 || text[0] != '0') {
    return -1;
  }
  for(i = 1; i < 4; i++) {
    if(text[i] < '0' || text[i] > '7') {
      return -1;
    }
  }
  *mode = (unsigned int)((text[1] - '0') << 6) |
          (unsigned int)((text[2] - '0') << 3) |
          (unsigned int)(text[3] - '0');
  return 0;
}

static int
chmod_task_path(file_task_t *task, const char *path,
                unsigned int mode, int recursive) {
  struct stat st;

  if(task_cancel_requested(task)) {
    return -1;
  }
  task_update(task, TASK_RUNNING, path, 0, NULL);
  if(lstat(path, &st)) {
    return -1;
  }
  if(recursive && S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;

    if(!dir) {
      return -1;
    }
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      int error;

      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if(task_cancel_requested(task) ||
         path_join(child, sizeof(child), path, entry->d_name) ||
         lstat(child, &st)) {
        error = errno;
        closedir(dir);
        errno = error;
        return -1;
      }
      /* A symlink has no portable mode of its own; never change its target. */
      if(!S_ISLNK(st.st_mode) && chmod_task_path(task, child, mode, 1)) {
        error = errno;
        closedir(dir);
        errno = error;
        return -1;
      }
    }
    closedir(dir);
  }
  task_update(task, TASK_RUNNING, path, 0, NULL);
  if(chmod_path_mode(path, mode)) {
    return -1;
  }
  task_update(task, TASK_RUNNING, path, 1, NULL);
  return 0;
}

static enum MHD_Result
api_chmod(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char *mode_text = body_form_value(body, body_size, "mode");
  char *recursive_text = body_form_value(body, body_size, "recursive");
  char **paths = NULL;
  size_t count = 0;
  unsigned int mode;
  enum MHD_Result ret;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(mode_text); free(recursive_text);
    free_paths(paths, count);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(parse_permission_mode(mode_text, &mode)) {
    free(paths_raw); free(mode_text); free(recursive_text);
    free_paths(paths, count);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "invalid permission mode",
                                  "invalid_mode", NULL);
  }
  ret = create_task_response(conn, TASK_CHMOD, paths, count, NULL, 0, mode,
                             recursive_text && !strcmp(recursive_text, "1"));
  free(paths_raw); free(mode_text); free(recursive_text);
  return ret;
}

#ifndef __linux__
static file_task_t *
next_pkg_task_locked(void) {
  file_task_t *task;
  file_task_t *next = NULL;

  for(task = g_tasks; task; task = task->next) {
    if(task->op == TASK_PKG_INSTALL && task->state == TASK_QUEUED &&
       (!next || task->id < next->id)) next = task;
  }
  return next;
}

static void *
pkg_task_worker(void *arg) {
  (void)arg;

  while(1) {
    file_task_t *task;
    int result;

    pthread_mutex_lock(&g_tasks_lock);
    while(!(task = next_pkg_task_locked())) {
      pthread_cond_wait(&g_pkg_tasks_cond, &g_tasks_lock);
    }
    task->state = TASK_RUNNING;
    snprintf(task->current, sizeof(task->current), "%s", task->src);
    task->updated_at = time(NULL);
    pthread_mutex_unlock(&g_tasks_lock);

    result = pkg_installer_install(task->srcs[0]);
    if(result) {
      char error_code[16];
      snprintf(error_code, sizeof(error_code), "0x%08X", (unsigned int)result);
      task_set_error_code(task, "pkg_install_failed", error_code);
      task_update(task, TASK_FAILED, task->src, 0,
                  "package installation failed");
    } else {
      task_update(task, TASK_DONE, task->src, 0, NULL);
    }
  }
  return NULL;
}

static file_task_t *
active_pkg_task_locked(const char *path) {
  file_task_t *task;

  for(task = g_tasks; task; task = task->next) {
    if(task->op == TASK_PKG_INSTALL && task_is_active(task) &&
       !strcmp(task->srcs[0], path)) return task;
  }
  return NULL;
}
#endif

static int
enqueue_pkg_tasks(char **paths, size_t count, unsigned long *ids) {
#ifdef __linux__
  (void)paths; (void)count; (void)ids;
  return PKG_INSTALL_UNSUPPORTED;
#else
  file_task_t **pending = calloc(count, sizeof(*pending));
  int result = 0;

  if(!pending) return -1;
  for(size_t i = 0; i < count; i++) {
    file_task_t *task = calloc(1, sizeof(*task));
    if(!task || !(task->srcs = calloc(1, sizeof(*task->srcs))) ||
       !(task->srcs[0] = strdup(paths[i]))) {
      free_task(task);
      result = -1;
      goto done;
    }
    atomic_init(&task->cancel_requested, 0);
    task->op = TASK_PKG_INSTALL;
    task->state = TASK_QUEUED;
    task->src_count = 1;
    snprintf(task->src, sizeof(task->src), "%s", paths[i]);
    task->created_at = time(NULL);
    task->updated_at = task->created_at;
    pending[i] = task;
  }

  pthread_mutex_lock(&g_tasks_lock);
  if(!g_pkg_worker_started) {
    result = pthread_create(&g_pkg_worker_thread, NULL, pkg_task_worker, NULL);
    if(!result) {
      pthread_detach(g_pkg_worker_thread);
      g_pkg_worker_started = 1;
    }
  }
  if(!result) {
    for(size_t i = 0; i < count; i++) {
      file_task_t *existing = active_pkg_task_locked(paths[i]);
      if(existing) {
        ids[i] = existing->id;
        free_task(pending[i]);
      } else {
        pending[i]->id = g_next_task_id++;
        ids[i] = pending[i]->id;
        pending[i]->next = g_tasks;
        g_tasks = pending[i];
      }
      pending[i] = NULL;
    }
    pthread_cond_signal(&g_pkg_tasks_cond);
  }
  pthread_mutex_unlock(&g_tasks_lock);

done:
  for(size_t i = 0; i < count; i++) free_task(pending[i]);
  free(pending);
  return result;
#endif
}

static enum MHD_Result
api_install_pkg(struct MHD_Connection *conn, const char *body,
                size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char **paths = NULL;
  unsigned long *ids = NULL;
  size_t count = 0;
  strbuf_t json = {0};
  int result;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count) ||
     !(ids = calloc(count, sizeof(*ids)))) {
    free(paths_raw); free_paths(paths, count); free(ids);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  for(size_t i = 0; i < count; i++) {
    const char *extension = strrchr(paths[i], '.');
    struct stat st;

    if(!extension || strcasecmp(extension, ".pkg")) {
      enum MHD_Result ret = send_json_error_detail(
        conn, MHD_HTTP_BAD_REQUEST, "file is not a PKG package",
        "pkg_type_invalid", paths[i]);
      free(paths_raw); free_paths(paths, count); free(ids);
      return ret;
    }
    if(stat(paths[i], &st) || !S_ISREG(st.st_mode)) {
      free(paths_raw); free_paths(paths, count); free(ids);
      return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
    }
  }

  result = enqueue_pkg_tasks(paths, count, ids);
  free(paths_raw); free_paths(paths, count);
  if(result == PKG_INSTALL_UNSUPPORTED) {
    free(ids);
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "package installation is only available on PS5",
                                  "pkg_install_unsupported", NULL);
  }
  if(result) {
    free(ids);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "could not queue package installation");
  }

  /* Keep the action from completing instantly after an accidental press. */
  usleep(600000);
  strbuf_append(&json, "{\"ok\":true,\"task_ids\":[");
  for(size_t i = 0; i < count; i++) {
    if(i) strbuf_append(&json, ",");
    strbuf_printf(&json, "%lu", ids[i]);
  }
  strbuf_append(&json, "]}");
  free(ids);
  return send_buffer(conn, MHD_HTTP_OK, json.data, "application/json");
}

enum MHD_Result
filemgr_api_request(struct MHD_Connection *conn, const char *url,
                    const char *method, const char *body, size_t body_size) {
  if((!strcmp(url, "/api/convert") || !strcmp(url, "/api/extract") ||
      !strcmp(url, "/api/url-download") || !strcmp(url, "/api/cancel") ||
      !strcmp(url, "/api/exit") || !strcmp(url, "/api/copy") ||
      !strcmp(url, "/api/move") || !strcmp(url, "/api/delete") ||
      !strcmp(url, "/api/upload/prepare") ||
      !strcmp(url, "/api/upload/finish") || !strcmp(url, "/api/rename") ||
      !strcmp(url, "/api/mkdir") || !strcmp(url, "/api/chmod") ||
      !strcmp(url, "/api/install-pkg") ||
      !strcmp(url, "/api/text/create") || !strcmp(url, "/api/text/save")) &&
     strcmp(method, MHD_HTTP_METHOD_POST)) {
    return send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method");
  }
  if(!strcmp(url, "/api/list")) return api_list(conn);
  if(!strcmp(url, "/api/roots")) return api_roots(conn);
  if(!strcmp(url, "/api/tasks")) return api_tasks(conn);
  if(!strcmp(url, "/api/convert")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ? send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") : api_convert(conn);
  }
  if(!strcmp(url, "/api/extract")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ? send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") : api_extract(conn);
  }
  if(!strcmp(url, "/api/url-download")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ? send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") : api_url_download(conn);
  }
  if(!strcmp(url, "/api/space")) return api_space(conn);
  if(!strcmp(url, "/api/cancel")) return api_cancel(conn);
  if(!strcmp(url, "/api/exit")) return api_exit(conn);
  if(!strcmp(url, "/api/copy")) return api_copy(conn, body, body_size);
  if(!strcmp(url, "/api/move")) return api_move(conn, body, body_size);
  if(!strcmp(url, "/api/delete")) return api_delete(conn, body, body_size);
  if(!strcmp(url, "/api/download/prepare")) return api_download_prepare(conn, body, body_size);
  if(!strcmp(url, "/api/download")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_download(conn);
  }
  if(!strcmp(url, "/api/upload/prepare")) return api_upload_prepare(conn, body, body_size);
  if(!strcmp(url, "/api/upload/finish")) return api_upload_finish(conn);
  if(!strcmp(url, "/api/rename")) return api_rename(conn);
  if(!strcmp(url, "/api/mkdir")) return api_mkdir(conn);
  if(!strcmp(url, "/api/chmod")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_chmod(conn, body, body_size);
  }
  if(!strcmp(url, "/api/pkg-info")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_pkg_info(conn);
  }
  if(!strcmp(url, "/api/pkg-icon")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_pkg_icon(conn);
  }
  if(!strcmp(url, "/api/install-pkg")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_install_pkg(conn, body, body_size);
  }
  if(!strcmp(url, "/api/text")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text(conn);
  }
  if(!strcmp(url, "/api/text/create")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text_create(conn);
  }
  if(!strcmp(url, "/api/text/save")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text_save(conn, body, body_size);
  }
  return send_json_error(conn, MHD_HTTP_NOT_FOUND, "unknown api");
}
