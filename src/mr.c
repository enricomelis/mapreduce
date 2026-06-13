#define _POSIX_C_SOURCE 200809L

#include "mr.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#define MR_DEFAULT_LOG_FILE "mr.log"
#define MR_LOG_MESSAGE_SIZE 512
#define MR_LOG_LINE_SIZE    768

#define MR_CHECK_NULL(attr)                                                                                            \
    do {                                                                                                               \
        if ((attr) == NULL) {                                                                                          \
            errno = EINVAL;                                                                                            \
            return -1;                                                                                                 \
        }                                                                                                              \
    } while (0)

struct mr {
    mr_attr_t attr;
    mr_mapper_t mapper;
    mr_reducer_t reducer;
    void *user_arg;
};

typedef struct {
    const char *file_name;
    size_t file_name_len;
    unsigned long line_number;
    const char *line;
    size_t line_len;
} mr_line_item_t;

typedef struct {
    int file_name_len;
    unsigned long line_number;
    int line_len;
} mr_line_header_t;

typedef struct {
    int token_len;
    int value_len;
} mr_pair_header_t;

typedef struct {
    int token_len;
    int result_len;
} mr_result_header_t;

typedef struct {
    char *full_path;
    char *file_name;
} mr_input_file_t;

typedef struct {
    int in_fd;
    int out_fd;
    mtx_t *lock;
} mr_log_collector_arg_t;

/* ==== helper generici ==== */

/*
 * Valori di ritorno:
 * - n: numero di byte richiesti letti completamente;
 * - 0: EOF prima di leggere qualsiasi byte;
 * - -1: errore o EOF dopo una lettura parziale.
 */
static ssize_t readn(int fd, void *buf, size_t n) {
    char *p = buf;
    size_t total = 0;

    while (total < n) {
        ssize_t act = read(fd, p + total, n - total);

        if (act > 0) {
            total += (size_t)act;
            continue;
        }

        if (act == 0) {
            if (total == 0) { return 0; }

            errno = EPROTO;
            return -1;
        }

        if (errno == EINTR) { continue; }

        return -1;
    }

    return (ssize_t)total;
}

static ssize_t writen(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t total = 0;

    while (total < n) {
        ssize_t act = write(fd, p + total, n - total);

        if (act > 0) {
            total += (size_t)act;
            continue;
        }

        if (act == 0) {
            errno = EIO;
            return -1;
        }

        if (errno == EINTR) { continue; }

        return -1;
    }

    return (ssize_t)total;
}

static int close_if_open(int *fd) {
    if (fd == NULL || *fd == -1) { return 0; }

    int current = *fd;
    *fd = -1;
    return close(current);
}

static int wait_if_started(pid_t *pid, int *status) {
    if (pid == NULL || *pid == -1) { return 0; }

    for (;;) {
        if (waitpid(*pid, status, 0) != -1) {
            *pid = -1;
            return 0;
        }

        if (errno == EINTR) { continue; }
        return -1;
    }
}

static void line_item_destroy(mr_line_item_t *item) {
    if (item == NULL) { return; }

    free((void *)item->file_name);
    free((void *)item->line);
    *item = (mr_line_item_t){ 0 };

    return;
}

typedef struct {
    mr_line_item_t *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int closed;
    mtx_t lock;
    cnd_t not_empty;
    cnd_t not_full;
} mr_line_queue_t;

static int line_queue_init(mr_line_queue_t *queue, size_t capacity) {
    MR_CHECK_NULL(queue);
    if (capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    if ((queue->items = calloc(capacity, sizeof(mr_line_item_t))) == NULL) { return -1; }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = 0;

    if (mtx_init(&queue->lock, mtx_plain) != thrd_success) {
        free(queue->items);
        return -1;
    }

    if (cnd_init(&queue->not_empty) != thrd_success) {
        mtx_destroy(&queue->lock);
        free(queue->items);
        return -1;
    }

    if (cnd_init(&queue->not_full) != thrd_success) {
        cnd_destroy(&queue->not_empty);
        mtx_destroy(&queue->lock);
        free(queue->items);
        return -1;
    }

    return 0;
}

static void line_queue_destroy(mr_line_queue_t *queue) {
    if (queue == NULL) { return; }

    if (queue->items != NULL && queue->capacity > 0) {
        for (size_t i = 0; i < queue->count; i++) {
            size_t index = (queue->head + i) % queue->capacity;
            line_item_destroy(&queue->items[index]);
        }
    }

    free(queue->items);
    cnd_destroy(&queue->not_empty);
    cnd_destroy(&queue->not_full);
    mtx_destroy(&queue->lock);

    queue->items = NULL;
    queue->capacity = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = 1;
}

static int line_queue_push(mr_line_queue_t *queue, mr_line_item_t *item) {
    MR_CHECK_NULL(queue);
    MR_CHECK_NULL(item);

    if (mtx_lock(&queue->lock) != thrd_success) { return -1; }

    while (queue->count == queue->capacity && !queue->closed) {
        if (cnd_wait(&queue->not_full, &queue->lock) != thrd_success) {
            mtx_unlock(&queue->lock);
            return -1;
        }
    }

    if (queue->closed) {
        mtx_unlock(&queue->lock);
        errno = EPIPE;
        return -1;
    }

    queue->items[queue->tail] = *item;
    *item = (mr_line_item_t){ 0 };
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    cnd_signal(&queue->not_empty);
    mtx_unlock(&queue->lock);

    return 0;
}

/*
 * Valori di ritorno:
 * - 1: elemento estratto dalla coda;
 * - 0: coda chiusa e vuota;
 * - -1: errore.
 */
static int line_queue_pop(mr_line_queue_t *queue, mr_line_item_t *item) {
    MR_CHECK_NULL(queue);
    MR_CHECK_NULL(item);

    if (mtx_lock(&queue->lock) != thrd_success) { return -1; }

    while (queue->count == 0 && !queue->closed) {
        if (cnd_wait(&queue->not_empty, &queue->lock) != thrd_success) {
            mtx_unlock(&queue->lock);
            return -1;
        }
    }

    if (queue->count == 0 && queue->closed) {
        mtx_unlock(&queue->lock);
        return 0;
    }

    *item = queue->items[queue->head];
    queue->items[queue->head] = (mr_line_item_t){ 0 };
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    cnd_signal(&queue->not_full);
    mtx_unlock(&queue->lock);

    return 1;
}

static void line_queue_close(mr_line_queue_t *queue) {
    if (queue == NULL) { return; }

    mtx_lock(&queue->lock);
    queue->closed = 1;
    cnd_broadcast(&queue->not_empty);
    cnd_broadcast(&queue->not_full);
    mtx_unlock(&queue->lock);
}

static int format_timestamp(char *buffer, size_t size) {
    MR_CHECK_NULL(buffer);
    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) { return -1; }

    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) { return -1; }

    if (strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    return 0;
}

static int log_message(int fd, mtx_t *lock, const char *process, const char *thread, const char *event, const char *fmt,
                       ...) {
    if (fd == -1 || process == NULL || thread == NULL || event == NULL || fmt == NULL) { return 0; }

    char timestamp[32];
    if (format_timestamp(timestamp, sizeof(timestamp)) == -1) { return -1; }

    char message[MR_LOG_MESSAGE_SIZE];
    va_list args;
    va_start(args, fmt);
    int message_len = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (message_len < 0) {
        errno = EIO;
        return -1;
    }

    char line[MR_LOG_LINE_SIZE];
    int line_len = snprintf(line, sizeof(line), "[%s] [%s:%ld] [%s] [%s] %s\n", timestamp, process, (long)getpid(),
                            thread, event, message);
    if (line_len < 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)line_len >= sizeof(line)) {
        errno = EOVERFLOW;
        return -1;
    }

    if (lock != NULL && mtx_lock(lock) != thrd_success) {
        errno = EIO;
        return -1;
    }

    int result = 0;
    int saved_errno = 0;
    if (writen(fd, line, (size_t)line_len) != line_len) {
        saved_errno = errno;
        result = -1;
    }

    if (lock != NULL && mtx_unlock(lock) != thrd_success && result == 0) {
        saved_errno = EIO;
        result = -1;
    }

    if (result == -1) { errno = saved_errno; }
    return result;
}

static int log_collector_main(void *arg) {
    MR_CHECK_NULL(arg);

    mr_log_collector_arg_t *collector = arg;
    char buffer[MR_LOG_LINE_SIZE];

    for (;;) {
        ssize_t n_read = read(collector->in_fd, buffer, sizeof(buffer));

        if (n_read == 0) { return 0; }
        if (n_read == -1) {
            if (errno == EINTR) { continue; }
            return -1;
        }

        if (mtx_lock(collector->lock) != thrd_success) {
            errno = EIO;
            return -1;
        }

        int result = 0;
        int saved_errno = 0;
        if (writen(collector->out_fd, buffer, (size_t)n_read) != n_read) {
            saved_errno = errno;
            result = -1;
        }

        if (mtx_unlock(collector->lock) != thrd_success && result == 0) {
            saved_errno = EIO;
            result = -1;
        }

        if (result == -1) {
            errno = saved_errno;
            return -1;
        }
    }
}

int mr_attr_init(mr_attr_t *attr) {
    MR_CHECK_NULL(attr);

    attr->mapper_threads = 1;
    attr->reducer_threads = 1;
    attr->queue_size = 64;
    attr->log_file = MR_DEFAULT_LOG_FILE;

    return 0;
}

int mr_attr_destroy(mr_attr_t *attr) {
    MR_CHECK_NULL(attr);

    attr->mapper_threads = 0;
    attr->reducer_threads = 0;
    attr->queue_size = 0;
    attr->log_file = NULL;

    return 0;
}

int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n) {
    MR_CHECK_NULL(attr);
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    attr->mapper_threads = n;
    return 0;
}

int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n) {
    MR_CHECK_NULL(attr);
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    attr->reducer_threads = n;
    return 0;
}

int mr_attr_set_queue_size(mr_attr_t *attr, size_t n) {
    MR_CHECK_NULL(attr);
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    attr->queue_size = n;
    return 0;
}

int mr_attr_set_log_file(mr_attr_t *attr, const char *path) {
    MR_CHECK_NULL(attr);

    attr->log_file = path != NULL ? path : MR_DEFAULT_LOG_FILE;
    return 0;
}

int mr_create(mr_t *mr, const mr_attr_t *attr, mr_mapper_t mapper, mr_reducer_t reducer, void *user_arg) {
    MR_CHECK_NULL(mr);
    MR_CHECK_NULL(attr);
    MR_CHECK_NULL(mapper);
    MR_CHECK_NULL(reducer);

    *mr = NULL;

    if (attr->mapper_threads == 0 || attr->reducer_threads == 0 || attr->queue_size == 0) {
        errno = EINVAL;
        return -1;
    }

    mr_t mapreduce = malloc(sizeof(*mapreduce));
    if (mapreduce == NULL) { return -1; }

    mapreduce->attr = *attr;
    if (mapreduce->attr.log_file == NULL) { mapreduce->attr.log_file = MR_DEFAULT_LOG_FILE; }
    mapreduce->mapper = mapper;
    mapreduce->reducer = reducer;
    mapreduce->user_arg = user_arg;

    *mr = mapreduce;
    return 0;
}

int mr_destroy(mr_t mr) {
    MR_CHECK_NULL(mr);

    free(mr);
    return 0;
}


/* ==== main verso mapper ==== */

static int write_line_record(int fd, const mr_line_item_t *item) {
    mr_line_header_t header;

    MR_CHECK_NULL(item);

    if (item->file_name_len > INT_MAX || item->line_len > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    if ((item->file_name_len > 0 && item->file_name == NULL) || (item->line_len > 0 && item->line == NULL)) {
        errno = EINVAL;
        return -1;
    }

    header.file_name_len = (int)item->file_name_len;
    header.line_number = item->line_number;
    header.line_len = (int)item->line_len;

    if (writen(fd, &header, sizeof(header)) != (ssize_t)sizeof(header)) { return -1; }

    if (item->file_name_len > 0 && writen(fd, item->file_name, item->file_name_len) != (ssize_t)item->file_name_len) {
        return -1;
    }

    if (item->line_len > 0 && writen(fd, item->line, item->line_len) != (ssize_t)item->line_len) { return -1; }

    return 0;
}

/*
 * Valori di ritorno:
 * - 1: record di riga letto e ricostruito;
 * - 0: EOF pulito;
 * - -1: errore o record troncato/non valido.
 */
static int read_line_record(int fd, mr_line_item_t *out) {
    mr_line_header_t header;

    MR_CHECK_NULL(out);

    ssize_t n_read = readn(fd, &header, sizeof(header));

    if (n_read == 0) { return 0; }
    if (n_read == -1) { return -1; }

    if (header.file_name_len < 0 || header.line_len < 0) {
        errno = EPROTO;
        return -1;
    }

    size_t file_name_len = (size_t)header.file_name_len;
    size_t line_len = (size_t)header.line_len;

    char *file_name;
    char *line;
    if ((file_name = malloc(file_name_len + 1)) == NULL) { return -1; }
    if ((line = malloc(line_len + 1)) == NULL) {
        free(file_name);
        return -1;
    }

    if (file_name_len > 0 && readn(fd, file_name, file_name_len) != (ssize_t)file_name_len) {
        free(file_name);
        free(line);
        return -1;
    }

    if (line_len > 0 && readn(fd, line, line_len) != (ssize_t)line_len) {
        free(file_name);
        free(line);
        return -1;
    }

    file_name[file_name_len] = '\0';
    line[line_len] = '\0';

    out->file_name = file_name;
    out->file_name_len = file_name_len;
    out->line_number = header.line_number;
    out->line = line;
    out->line_len = line_len;

    return 1;
}

static int write_file_lines(int out_fd, const char *path, const char *file_name, int log_fd, mtx_t *log_lock,
                            size_t *lines_sent) {
    MR_CHECK_NULL(path);
    MR_CHECK_NULL(file_name);

    log_message(log_fd, log_lock, "main", "main", "FILE_OPEN", "input path=%s", path);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { return -1; }

    mr_line_item_t item = { 0 };

    unsigned long line_num = 1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n_read = 0;
    size_t file_name_len = strlen(file_name);

    while ((n_read = getline(&line, &capacity, fp)) != -1) {
        size_t line_len = (size_t)n_read;
        if (line_len > 0 && line[line_len - 1] == '\n') { line_len--; }

        item.file_name = file_name;
        item.file_name_len = file_name_len;
        item.line = line;
        item.line_len = line_len;
        item.line_number = line_num;

        line_num++;

        if (write_line_record(out_fd, &item) == -1) {
            int saved_errno = errno;
            free(line);
            fclose(fp);
            errno = saved_errno;
            return -1;
        }

        if (lines_sent != NULL) { (*lines_sent)++; }
    }

    if (ferror(fp)) {
        int saved_errno = errno != 0 ? errno : EIO;
        free(line);
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    free(line);
    if (fclose(fp) == EOF) { return -1; }
    log_message(log_fd, log_lock, "main", "main", "FILE_CLOSE", "input path=%s", path);

    return 0;
}

static void input_files_destroy(mr_input_file_t *files, size_t count) {
    if (files == NULL) { return; }

    for (size_t i = 0; i < count; i++) {
        free(files[i].full_path);
        free(files[i].file_name);
    }

    free(files);
}

static int input_file_compare(const void *left, const void *right) {
    const mr_input_file_t *a = left;
    const mr_input_file_t *b = right;

    return strcmp(a->file_name, b->file_name);
}

static const char *input_path_basename(const char *path) {
    const char *slash = strrchr(path, '/');

    if (slash == NULL) { return path; }
    return slash + 1;
}

static int build_full_path(const char *directory, const char *file_name, char **out) {
    MR_CHECK_NULL(directory);
    MR_CHECK_NULL(file_name);
    MR_CHECK_NULL(out);

    size_t directory_len = strlen(directory);
    size_t file_name_len = strlen(file_name);
    int add_slash = directory_len > 0 && directory[directory_len - 1] != '/';

    if (directory_len > SIZE_MAX - file_name_len - (size_t)add_slash - 1) {
        errno = EOVERFLOW;
        return -1;
    }

    size_t full_path_len = directory_len + (size_t)add_slash + file_name_len;
    char *full_path = malloc(full_path_len + 1);
    if (full_path == NULL) { return -1; }

    memcpy(full_path, directory, directory_len);
    if (add_slash) { full_path[directory_len] = '/'; }
    memcpy(full_path + directory_len + (size_t)add_slash, file_name, file_name_len);
    full_path[full_path_len] = '\0';

    *out = full_path;
    return 0;
}

static int input_files_push(mr_input_file_t **files, size_t *count, size_t *capacity, const char *full_path,
                            const char *file_name) {
    MR_CHECK_NULL(files);
    MR_CHECK_NULL(count);
    MR_CHECK_NULL(capacity);
    MR_CHECK_NULL(full_path);
    MR_CHECK_NULL(file_name);

    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(**files)) {
            errno = EOVERFLOW;
            return -1;
        }

        mr_input_file_t *new_files = realloc(*files, new_capacity * sizeof(*new_files));
        if (new_files == NULL) { return -1; }

        *files = new_files;
        *capacity = new_capacity;
    }

    char *full_path_copy = strdup(full_path);
    if (full_path_copy == NULL) { return -1; }

    char *file_name_copy = strdup(file_name);
    if (file_name_copy == NULL) {
        free(full_path_copy);
        return -1;
    }

    (*files)[*count].full_path = full_path_copy;
    (*files)[*count].file_name = file_name_copy;
    (*count)++;

    return 0;
}

static int write_directory_lines(int out_fd, const char *input_path, int log_fd, mtx_t *log_lock, size_t *lines_sent) {
    DIR *dir = opendir(input_path);
    if (dir == NULL) { return -1; }

    mr_input_file_t *files = NULL;
    size_t count = 0;
    size_t capacity = 0;

    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) { continue; }

        char *full_path = NULL;
        if (build_full_path(input_path, entry->d_name, &full_path) == -1) {
            int saved_errno = errno;
            input_files_destroy(files, count);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        struct stat st;
        if (stat(full_path, &st) == -1) {
            int saved_errno = errno;
            free(full_path);
            input_files_destroy(files, count);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        if (S_ISREG(st.st_mode) && input_files_push(&files, &count, &capacity, full_path, entry->d_name) == -1) {
            int saved_errno = errno;
            free(full_path);
            input_files_destroy(files, count);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        free(full_path);
        errno = 0;
    }

    if (errno != 0) {
        int saved_errno = errno;
        input_files_destroy(files, count);
        closedir(dir);
        errno = saved_errno;
        return -1;
    }

    if (closedir(dir) == -1) {
        int saved_errno = errno;
        input_files_destroy(files, count);
        errno = saved_errno;
        return -1;
    }

    qsort(files, count, sizeof(*files), input_file_compare);

    for (size_t i = 0; i < count; i++) {
        if (write_file_lines(out_fd, files[i].full_path, files[i].file_name, log_fd, log_lock, lines_sent) == -1) {
            int saved_errno = errno;
            input_files_destroy(files, count);
            errno = saved_errno;
            return -1;
        }
    }

    input_files_destroy(files, count);
    return 0;
}

static int write_input_path_lines(int out_fd, const char *input_path, int log_fd, mtx_t *log_lock, size_t *lines_sent) {
    MR_CHECK_NULL(input_path);
    if (lines_sent != NULL) { *lines_sent = 0; }

    struct stat st;
    if (stat(input_path, &st) == -1) { return -1; }

    if (S_ISREG(st.st_mode)) {
        return write_file_lines(out_fd, input_path, input_path_basename(input_path), log_fd, log_lock, lines_sent);
    }

    if (S_ISDIR(st.st_mode)) { return write_directory_lines(out_fd, input_path, log_fd, log_lock, lines_sent); }

    errno = EINVAL;
    return -1;
}


/* ==== mapper verso reducer (scrittura) ==== */

static int is_valid_token(const char *token) {
    if (token == NULL || token[0] == '\0') { return 0; }

    for (const char *p = token; *p != '\0'; p++) {
        int is_digit = *p >= '0' && *p <= '9';
        int is_upper = *p >= 'A' && *p <= 'Z';
        int is_lower = *p >= 'a' && *p <= 'z';

        if (!is_digit && !is_upper && !is_lower) { return 0; }
    }

    return 1;
}

typedef struct {
    mr_line_queue_t queue;
    mr_mapper_t mapper;
    void *user_arg;
    int out_fd;
    int log_fd;
    size_t pairs_produced;
    mtx_t pipe_emit_lock;
    mtx_t log_lock;
} mr_mapper_context_t;

typedef struct {
    mr_mapper_context_t *context;
    size_t index;
} mr_mapper_worker_arg_t;

static int mapper_emit_pair(const char *token, const void *value, size_t value_size, void *emit_arg) {
    MR_CHECK_NULL(emit_arg);
    mr_mapper_context_t *context = emit_arg;

    if (!is_valid_token(token) || (value_size > 0 && value == NULL)) {
        errno = EINVAL;
        return -1;
    }

    size_t token_len = strlen(token);
    if (token_len > INT_MAX || value_size > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    mr_pair_header_t header = {
        .token_len = (int)token_len,
        .value_len = (int)value_size,
    };

    if (mtx_lock(&context->pipe_emit_lock) != thrd_success) {
        errno = EIO;
        return -1;
    }

    int saved_errno = 0;

    if (writen(context->out_fd, &header, sizeof(header)) != (ssize_t)sizeof(header)) {
        saved_errno = errno;
        mtx_unlock(&context->pipe_emit_lock);
        errno = saved_errno;
        return -1;
    }

    if (writen(context->out_fd, token, token_len) != (ssize_t)token_len) {
        saved_errno = errno;
        mtx_unlock(&context->pipe_emit_lock);
        errno = saved_errno;
        return -1;
    }

    if (value_size > 0 && writen(context->out_fd, value, value_size) != (ssize_t)value_size) {
        saved_errno = errno;
        mtx_unlock(&context->pipe_emit_lock);
        errno = saved_errno;
        return -1;
    }

    context->pairs_produced++;
    mtx_unlock(&context->pipe_emit_lock);

    return 0;
}


/* ==== mapper ==== */

static int mapper_reader_main(void *arg) {
    MR_CHECK_NULL(arg);
    mr_mapper_context_t *context = arg;

    log_message(context->log_fd, &context->log_lock, "mapper", "reader", "THREAD_START", "mapper reader started");

    mr_line_item_t item = { 0 };

    for (;;) {
        item = (mr_line_item_t){ 0 };
        int queue_status = read_line_record(STDIN_FILENO, &item);

        if (queue_status == -1) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", "reader", "ERROR", "mapper reader failed");
            return -1;
        }

        if (queue_status == 0) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", "reader", "THREAD_END", "mapper reader ended");
            return 0;
        }

        if (queue_status == 1) {
            if (line_queue_push(&context->queue, &item) == -1) {
                line_item_destroy(&item);
                line_queue_close(&context->queue);
                log_message(context->log_fd, &context->log_lock, "mapper", "reader", "ERROR",
                            "mapper reader queue push failed");
                return -1;
            }
        }
    }
    return 0;
}

static int mapper_worker_main(void *arg) {
    MR_CHECK_NULL(arg);
    mr_mapper_worker_arg_t *worker = arg;
    mr_mapper_context_t *context = worker->context;
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "worker-%zu", worker->index);

    log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "THREAD_START", "mapper worker started");

    for (;;) {
        mr_line_item_t item = { 0 };
        int queue_status = line_queue_pop(&context->queue, &item);

        if (queue_status == -1) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "ERROR",
                        "mapper worker queue pop failed");
            return -1;
        }

        if (queue_status == 0) {
            log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "THREAD_END",
                        "mapper worker ended");
            return 0;
        }

        mr_file_line_t line = { .file_name = item.file_name,
                                .file_name_len = item.file_name_len,
                                .line = item.line,
                                .line_len = item.line_len,
                                .line_number = item.line_number };

        int mapper_status = context->mapper(&line, mapper_emit_pair, context, context->user_arg);

        line_item_destroy(&item);

        if (mapper_status == -1) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "ERROR", "mapper callback failed");
            return -1;
        }
    }

    return 0;
}

static int mapper_process_main(mr_t mr, int log_fd) {
    MR_CHECK_NULL(mr);

    mr_mapper_context_t context = { 0 };
    context.mapper = mr->mapper;
    context.user_arg = mr->user_arg;
    context.out_fd = STDOUT_FILENO;
    context.log_fd = log_fd;

    if (line_queue_init(&context.queue, mr->attr.queue_size) == -1) { return -1; }

    if (mtx_init(&context.pipe_emit_lock, mtx_plain) != thrd_success) {
        line_queue_destroy(&context.queue);
        return -1;
    }

    if (mtx_init(&context.log_lock, mtx_plain) != thrd_success) {
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    thrd_t *worker_threads = malloc(mr->attr.mapper_threads * sizeof(*worker_threads));
    if (worker_threads == NULL) {
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    mr_mapper_worker_arg_t *worker_args = malloc(mr->attr.mapper_threads * sizeof(*worker_args));
    if (worker_args == NULL) {
        free(worker_threads);
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    size_t workers_created = 0;
    int result = 0;

    for (size_t i = 0; i < mr->attr.mapper_threads; i++) {
        worker_args[i].context = &context;
        worker_args[i].index = i;
        if (thrd_create(&worker_threads[i], mapper_worker_main, &worker_args[i]) != thrd_success) {
            result = -1;
            line_queue_close(&context.queue);
            break;
        }
        workers_created++;
    }

    int status;
    if (result == -1) {
        for (size_t i = 0; i < workers_created; i++) {
            if (thrd_join(worker_threads[i], &status) != thrd_success) { result = -1; }
        }

        free(worker_args);
        free(worker_threads);
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    thrd_t reader_thread;
    if (thrd_create(&reader_thread, mapper_reader_main, &context) != thrd_success) {
        line_queue_close(&context.queue);

        for (size_t i = 0; i < workers_created; i++) {
            if (thrd_join(worker_threads[i], &status) != thrd_success) { result = -1; }
        }

        free(worker_args);
        free(worker_threads);
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    if (thrd_join(reader_thread, &status) != thrd_success || status != 0) { result = -1; }

    for (size_t i = 0; i < workers_created; i++) {
        if (thrd_join(worker_threads[i], &status) != thrd_success || status != 0) { result = -1; }
    }

    log_message(context.log_fd, &context.log_lock, "mapper", "main", "COUNT", "pairs_produced=%zu",
                context.pairs_produced);

    free(worker_args);
    free(worker_threads);
    mtx_destroy(&context.log_lock);
    mtx_destroy(&context.pipe_emit_lock);
    line_queue_destroy(&context.queue);

    return result;
}


/* ==== mapper verso reducer (lettura) ==== */

typedef struct {
    char *token;
    size_t token_len;
    void *value;
    size_t value_len;
} mr_pair_item_t;

static void pair_item_destroy(mr_pair_item_t *item) {
    if (item == NULL) { return; }

    free(item->token);
    free(item->value);

    *item = (mr_pair_item_t){ 0 };

    return;
}

/*
 * Valori di ritorno:
 * - 1: coppia intermedia letta e ricostruita;
 * - 0: EOF pulito;
 * - -1: errore o record troncato/non valido.
 */
static int read_pair_record(int fd, mr_pair_item_t *item_out) {
    MR_CHECK_NULL(item_out);
    *item_out = (mr_pair_item_t){ 0 };

    mr_pair_header_t header;

    ssize_t n_read = readn(fd, &header, sizeof(header));

    if (n_read == 0) { return 0; }
    if (n_read == -1) { return -1; }

    if (header.token_len <= 0 || header.value_len < 0) {
        errno = EPROTO;
        return -1;
    }

    size_t token_len = (size_t)header.token_len;
    size_t value_len = (size_t)header.value_len;

    char *token = malloc(token_len + 1);
    if (token == NULL) { return -1; }

    void *value = NULL;
    if (value_len > 0) {
        value = malloc(value_len);
        if (value == NULL) {
            int saved_errno = errno;
            free(token);
            errno = saved_errno;
            return -1;
        }
    }

    n_read = readn(fd, token, token_len);
    if (n_read != (ssize_t)token_len) {
        free(token);
        free(value);
        if (n_read == 0) { errno = EPROTO; }
        return -1;
    }

    token[token_len] = '\0';

    if (value_len > 0) {
        n_read = readn(fd, value, value_len);
        if (n_read != (ssize_t)value_len) {
            free(token);
            free(value);
            if (n_read == 0) { errno = EPROTO; }
            return -1;
        }
    }

    item_out->token = token;
    item_out->token_len = token_len;
    item_out->value = value;
    item_out->value_len = value_len;

    return 1;
}

/* ==== reducer verso main ==== */

static int write_reducer_results(int in_fd, const char *output_path, int log_fd, mtx_t *log_lock,
                                 size_t *results_written) {
    MR_CHECK_NULL(output_path);
    if (results_written != NULL) { *results_written = 0; }

    log_message(log_fd, log_lock, "main", "main", "FILE_OPEN", "output path=%s", output_path);
    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd == -1) { return -1; }

    int result = 0;
    int saved_errno = 0;

    for (;;) {
        mr_result_header_t header;
        ssize_t n_read = readn(in_fd, &header, sizeof(header));

        if (n_read == 0) { break; }
        if (n_read == -1) {
            saved_errno = errno;
            result = -1;
            break;
        }

        if (header.token_len <= 0 || header.result_len < 0) {
            saved_errno = EPROTO;
            result = -1;
            break;
        }

        size_t token_len = (size_t)header.token_len;
        size_t result_len = (size_t)header.result_len;

        char *token = malloc(token_len);
        if (token == NULL) {
            saved_errno = errno;
            result = -1;
            break;
        }

        void *payload = NULL;
        if (result_len > 0) {
            payload = malloc(result_len);
            if (payload == NULL) {
                saved_errno = errno;
                free(token);
                result = -1;
                break;
            }
        }

        n_read = readn(in_fd, token, token_len);
        if (n_read != (ssize_t)token_len) {
            saved_errno = n_read == -1 ? errno : EPROTO;
            free(token);
            free(payload);
            result = -1;
            break;
        }

        if (result_len > 0) {
            n_read = readn(in_fd, payload, result_len);
            if (n_read != (ssize_t)result_len) {
                saved_errno = n_read == -1 ? errno : EPROTO;
                free(token);
                free(payload);
                result = -1;
                break;
            }
        }

        if (writen(out_fd, &header, sizeof(header)) != (ssize_t)sizeof(header) ||
            writen(out_fd, token, token_len) != (ssize_t)token_len ||
            (result_len > 0 && writen(out_fd, payload, result_len) != (ssize_t)result_len)) {
            saved_errno = errno;
            free(token);
            free(payload);
            result = -1;
            break;
        }

        if (results_written != NULL) { (*results_written)++; }

        free(token);
        free(payload);
    }

    if (close(out_fd) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_fd, log_lock, "main", "main", "FILE_CLOSE", "output path=%s", output_path);
    }

    if (result == -1) { errno = saved_errno; }
    return result;
}


typedef struct {
    char *token;
    size_t token_len;
    void *data;
    size_t size;
} mr_result_item_t;

typedef struct {
    mr_result_item_t *items;
    size_t count;
    size_t capacity;
} mr_result_list_t;

static void result_list_destroy(mr_result_list_t *list) {
    if (list == NULL) { return; }

    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].token);
        free(list->items[i].data);
    }

    free(list->items);
    *list = (mr_result_list_t){ 0 };
}

static int result_list_push(mr_result_list_t *list, const char *token, const void *result, size_t result_size) {
    MR_CHECK_NULL(list);
    if (!is_valid_token(token) || (result_size > 0 && result == NULL)) {
        errno = EINVAL;
        return -1;
    }

    size_t token_len = strlen(token);
    if (token_len > INT_MAX || result_size > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;

        if (new_capacity < list->capacity || new_capacity > SIZE_MAX / sizeof(*list->items)) {
            errno = ENOMEM;
            return -1;
        }

        mr_result_item_t *new_items = realloc(list->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) { return -1; }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    char *token_copy = malloc(token_len);
    if (token_copy == NULL) { return -1; }

    void *data_copy = NULL;
    if (result_size > 0) {
        data_copy = malloc(result_size);
        if (data_copy == NULL) {
            int saved_errno = errno;
            free(token_copy);
            errno = saved_errno;
            return -1;
        }
        memcpy(data_copy, result, result_size);
    }

    memcpy(token_copy, token, token_len);

    mr_result_item_t *item = &list->items[list->count];
    item->token = token_copy;
    item->token_len = token_len;
    item->data = data_copy;
    item->size = result_size;
    list->count++;

    return 0;
}

static int reducer_collect_result(const char *token, const void *result, size_t result_size, void *emit_arg) {
    return result_list_push(emit_arg, token, result, result_size);
}

static int write_result_list(int fd, const mr_result_list_t *list) {
    MR_CHECK_NULL(list);

    for (size_t i = 0; i < list->count; i++) {
        const mr_result_item_t *item = &list->items[i];
        mr_result_header_t header = {
            .token_len = (int)item->token_len,
            .result_len = (int)item->size,
        };

        if (writen(fd, &header, sizeof(header)) != (ssize_t)sizeof(header)) { return -1; }
        if (writen(fd, item->token, item->token_len) != (ssize_t)item->token_len) { return -1; }
        if (item->size > 0 && writen(fd, item->data, item->size) != (ssize_t)item->size) { return -1; }
    }

    return 0;
}

/* ==== reducer ==== */

typedef struct {
    char *token;
    size_t token_len;
    mr_value_t *values;
    size_t values_count;
    size_t values_capacity;
    mr_result_list_t results;
} mr_pair_group_t;

typedef struct {
    mr_pair_group_t *items;
    size_t count;
    size_t capacity;
} mr_pair_groups_t;

static void pair_group_destroy(mr_pair_group_t *group) {
    if (group == NULL) { return; }

    free(group->token);
    for (size_t i = 0; i < group->values_count; i++) {
        free((void *)group->values[i].data);
    }
    free(group->values);
    result_list_destroy(&group->results);

    *group = (mr_pair_group_t){ 0 };

    return;
}

static void pair_groups_destroy(mr_pair_groups_t *groups) {
    if (groups == NULL) { return; }

    for (size_t i = 0; i < groups->count; i++) {
        pair_group_destroy(&groups->items[i]);
    }
    free(groups->items);

    *groups = (mr_pair_groups_t){ 0 };

    return;
}

static mr_pair_group_t *pair_groups_find(mr_pair_groups_t *groups, const char *token, size_t token_len) {
    if (groups == NULL || token == NULL) { return NULL; }

    for (size_t i = 0; i < groups->count; i++) {
        if (groups->items[i].token_len == token_len && memcmp(groups->items[i].token, token, token_len) == 0) {
            return &groups->items[i];
        }
    }

    return NULL;
}

static int pair_group_compare(const void *left, const void *right) {
    const mr_pair_group_t *a = left;
    const mr_pair_group_t *b = right;
    size_t min_len = a->token_len < b->token_len ? a->token_len : b->token_len;
    int cmp = memcmp(a->token, b->token, min_len);

    if (cmp != 0) { return cmp; }
    if (a->token_len < b->token_len) { return -1; }
    if (a->token_len > b->token_len) { return 1; }
    return 0;
}

static mr_pair_group_t *pair_groups_push_group(mr_pair_groups_t *groups, mr_pair_item_t *item) {
    if (groups == NULL || item == NULL || item->token == NULL || item->token_len == 0) {
        errno = EINVAL;
        return NULL;
    }

    if (groups->count == groups->capacity) {
        size_t new_capacity = groups->capacity == 0 ? 8 : groups->capacity * 2;

        if (new_capacity < groups->capacity || new_capacity > SIZE_MAX / sizeof(*groups->items)) {
            errno = ENOMEM;
            return NULL;
        }

        mr_pair_group_t *new_items = realloc(groups->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) { return NULL; }

        groups->items = new_items;
        groups->capacity = new_capacity;
    }

    mr_pair_group_t *group = &groups->items[groups->count];
    *group = (mr_pair_group_t){ 0 };
    group->token = item->token;
    group->token_len = item->token_len;
    groups->count++;

    item->token = NULL;
    item->token_len = 0;

    return group;
}

static int pair_group_add_value(mr_pair_group_t *group, mr_pair_item_t *item) {
    MR_CHECK_NULL(group);
    MR_CHECK_NULL(item);
    if (item->value_len > 0 && item->value == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (group->values_count == group->values_capacity) {
        size_t new_capacity = group->values_capacity == 0 ? 8 : group->values_capacity * 2;

        if (new_capacity < group->values_capacity || new_capacity > SIZE_MAX / sizeof(*group->values)) {
            errno = ENOMEM;
            return -1;
        }

        mr_value_t *new_values = realloc(group->values, new_capacity * sizeof(*new_values));
        if (new_values == NULL) { return -1; }

        group->values = new_values;
        group->values_capacity = new_capacity;
    }

    mr_value_t *value = &group->values[group->values_count];
    value->data = item->value;
    value->size = item->value_len;
    group->values_count++;

    item->value = NULL;
    item->value_len = 0;

    return 0;
}

static int pair_groups_add_pair(mr_pair_groups_t *groups, mr_pair_item_t *pair) {
    MR_CHECK_NULL(groups);
    MR_CHECK_NULL(pair);
    if (pair->token == NULL || pair->token_len == 0 || (pair->value_len > 0 && pair->value == NULL)) {
        errno = EINVAL;
        return -1;
    }

    int created_group = 0;
    mr_pair_group_t *group = pair_groups_find(groups, pair->token, pair->token_len);

    if (group == NULL) {
        group = pair_groups_push_group(groups, pair);
        if (group == NULL) { return -1; }
        created_group = 1;
    }

    if (pair_group_add_value(group, pair) == -1) {
        if (created_group != 0) {
            pair->token = group->token;
            pair->token_len = group->token_len;
            *group = (mr_pair_group_t){ 0 };
            groups->count--;
        }
        return -1;
    }

    if (created_group == 0) {
        free(pair->token);
        pair->token = NULL;
        pair->token_len = 0;
    }

    return 0;
}

static int collect_pair_groups(int fd, mr_pair_groups_t *groups) {
    MR_CHECK_NULL(groups);

    for (;;) {
        mr_pair_item_t pair = { 0 };
        int status = read_pair_record(fd, &pair);

        if (status == 0) { return 0; }
        if (status == -1) {
            pair_item_destroy(&pair);
            return -1;
        }

        if (pair_groups_add_pair(groups, &pair) == -1) {
            pair_item_destroy(&pair);
            return -1;
        }

        pair_item_destroy(&pair);
    }
}

typedef struct {
    mr_pair_groups_t *groups;
    mr_reducer_t reducer;
    void *user_arg;
    int log_fd;
    mtx_t *log_lock;
    size_t next_index;
    int result;
    int saved_errno;
    mtx_t lock;
} mr_reducer_worker_context_t;

typedef struct {
    mr_reducer_worker_context_t *context;
    size_t index;
} mr_reducer_worker_arg_t;

static int reducer_worker_main(void *arg) {
    MR_CHECK_NULL(arg);
    mr_reducer_worker_arg_t *worker = arg;
    mr_reducer_worker_context_t *context = worker->context;
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "worker-%zu", worker->index);

    log_message(context->log_fd, context->log_lock, "reducer", thread_name, "THREAD_START", "reducer worker started");

    for (;;) {
        if (mtx_lock(&context->lock) != thrd_success) {
            errno = EIO;
            log_message(context->log_fd, context->log_lock, "reducer", thread_name, "ERROR",
                        "reducer worker lock failed");
            return -1;
        }

        if (context->result == -1 || context->next_index == context->groups->count) {
            int result = context->result;
            mtx_unlock(&context->lock);
            log_message(context->log_fd, context->log_lock, "reducer", thread_name, "THREAD_END",
                        "reducer worker ended");
            return result;
        }

        size_t index = context->next_index;
        context->next_index++;
        mtx_unlock(&context->lock);

        mr_pair_group_t *group = &context->groups->items[index];
        int status = context->reducer(group->token, group->values, group->values_count, reducer_collect_result,
                                      &group->results, context->user_arg);

        if (status == -1) {
            int saved_errno = errno;

            if (mtx_lock(&context->lock) == thrd_success) {
                if (context->result == 0) {
                    context->saved_errno = saved_errno;
                    context->result = -1;
                }
                mtx_unlock(&context->lock);
            }

            errno = saved_errno;
            log_message(context->log_fd, context->log_lock, "reducer", thread_name, "ERROR", "reducer callback failed");
            return -1;
        }
    }
}

static int reducer_process_main(mr_t mr, int log_fd) {
    MR_CHECK_NULL(mr);

    mr_pair_groups_t groups = { 0 };
    int result = 0;
    int saved_errno = 0;
    size_t results_produced = 0;
    mtx_t log_lock;

    if (collect_pair_groups(STDIN_FILENO, &groups) == -1) {
        saved_errno = errno;
        pair_groups_destroy(&groups);
        errno = saved_errno;
        return -1;
    }

    if (mtx_init(&log_lock, mtx_plain) != thrd_success) {
        pair_groups_destroy(&groups);
        errno = EIO;
        return -1;
    }

    log_message(log_fd, &log_lock, "reducer", "main", "COUNT", "distinct_tokens=%zu", groups.count);

    qsort(groups.items, groups.count, sizeof(*groups.items), pair_group_compare);

    mr_reducer_worker_context_t context = {
        .groups = &groups,
        .reducer = mr->reducer,
        .user_arg = mr->user_arg,
        .log_fd = log_fd,
        .log_lock = &log_lock,
        .next_index = 0,
        .result = 0,
        .saved_errno = 0,
    };

    if (mtx_init(&context.lock, mtx_plain) != thrd_success) {
        mtx_destroy(&log_lock);
        pair_groups_destroy(&groups);
        errno = EIO;
        return -1;
    }

    thrd_t *worker_threads = malloc(mr->attr.reducer_threads * sizeof(*worker_threads));
    if (worker_threads == NULL) {
        saved_errno = errno;
        mtx_destroy(&context.lock);
        mtx_destroy(&log_lock);
        pair_groups_destroy(&groups);
        errno = saved_errno;
        return -1;
    }

    mr_reducer_worker_arg_t *worker_args = malloc(mr->attr.reducer_threads * sizeof(*worker_args));
    if (worker_args == NULL) {
        saved_errno = errno;
        free(worker_threads);
        mtx_destroy(&context.lock);
        mtx_destroy(&log_lock);
        pair_groups_destroy(&groups);
        errno = saved_errno;
        return -1;
    }

    size_t workers_created = 0;

    for (size_t i = 0; i < mr->attr.reducer_threads; i++) {
        worker_args[i].context = &context;
        worker_args[i].index = i;
        if (thrd_create(&worker_threads[i], reducer_worker_main, &worker_args[i]) != thrd_success) {
            saved_errno = EIO;
            result = -1;
            break;
        }
        workers_created++;
    }

    if (result == -1) {
        if (mtx_lock(&context.lock) == thrd_success) {
            context.result = -1;
            if (context.saved_errno == 0) { context.saved_errno = saved_errno; }
            mtx_unlock(&context.lock);
        }
    }

    int join_failed = 0;
    int worker_failed = 0;

    for (size_t i = 0; i < workers_created; i++) {
        int status = 0;
        if (thrd_join(worker_threads[i], &status) != thrd_success) {
            join_failed = 1;
        } else if (status != 0) {
            worker_failed = 1;
        }
    }

    if (result == 0 && join_failed != 0) {
        saved_errno = EIO;
        result = -1;
    }

    if (result == 0 && (worker_failed != 0 || context.result == -1)) {
        saved_errno = context.saved_errno != 0 ? context.saved_errno : EIO;
        result = -1;
    }

    if (result == 0) {
        for (size_t i = 0; i < groups.count; i++) {
            results_produced += groups.items[i].results.count;
            if (write_result_list(STDOUT_FILENO, &groups.items[i].results) == -1) {
                saved_errno = errno;
                result = -1;
                break;
            }
        }
    }

    if (result == 0) {
        log_message(log_fd, &log_lock, "reducer", "main", "COUNT", "results_produced=%zu", results_produced);
    }

    free(worker_args);
    free(worker_threads);
    pair_groups_destroy(&groups);
    mtx_destroy(&context.lock);
    mtx_destroy(&log_lock);

    if (result == -1) { errno = saved_errno; }
    return result;
}


/* ==== main ==== */

int mr_start(mr_t mr, const char *input_path, const char *output_path) {
    MR_CHECK_NULL(mr);
    MR_CHECK_NULL(input_path);
    MR_CHECK_NULL(output_path);

    int main_to_mapper[2] = { -1, -1 };
    int mapper_to_reducer[2] = { -1, -1 };
    int reducer_to_main[2] = { -1, -1 };
    int mapper_log_to_main[2] = { -1, -1 };
    int reducer_log_to_main[2] = { -1, -1 };
    int log_file_fd = -1;
    mtx_t log_file_lock;
    int log_file_lock_ready = 0;
    thrd_t mapper_log_thread;
    thrd_t reducer_log_thread;
    int mapper_log_thread_started = 0;
    int reducer_log_thread_started = 0;
    mr_log_collector_arg_t mapper_log_arg = { 0 };
    mr_log_collector_arg_t reducer_log_arg = { 0 };
    pid_t mapper_pid = -1;
    pid_t reducer_pid = -1;
    int mapper_status = 0;
    int reducer_status = 0;
    int result = 0;
    int saved_errno = 0;
    size_t lines_sent = 0;
    size_t results_written = 0;

    log_file_fd = open(mr->attr.log_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_file_fd == -1) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0 && mtx_init(&log_file_lock, mtx_plain) != thrd_success) {
        saved_errno = EIO;
        result = -1;
    } else if (result == 0) {
        log_file_lock_ready = 1;
    }

    if (result == 0 && pipe(main_to_mapper) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created main_to_mapper");
    }

    if (result == 0 && pipe(mapper_to_reducer) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created mapper_to_reducer");
    }

    if (result == 0 && pipe(reducer_to_main) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created reducer_to_main");
    }

    if (result == 0 && pipe(mapper_log_to_main) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created mapper_log_to_main");
    }

    if (result == 0 && pipe(reducer_log_to_main) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created reducer_log_to_main");
    }

    if (result == 0) {
        mapper_pid = fork();

        if (mapper_pid == -1) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (result == 0 && mapper_pid == 0) {
        close_if_open(&log_file_fd);
        if (dup2(main_to_mapper[0], STDIN_FILENO) == -1) { _exit(1); }
        if (dup2(mapper_to_reducer[1], STDOUT_FILENO) == -1) { _exit(1); }

        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);
        close_if_open(&mapper_log_to_main[0]);
        close_if_open(&reducer_log_to_main[0]);
        close_if_open(&reducer_log_to_main[1]);

        log_message(mapper_log_to_main[1], NULL, "mapper", "main", "PROCESS_START", "mapper process started");
        int mapper_status = mapper_process_main(mr, mapper_log_to_main[1]);
        log_message(mapper_log_to_main[1], NULL, "mapper", "main", "PROCESS_END", "mapper process ended status=%d",
                    mapper_status);
        close_if_open(&mapper_log_to_main[1]);
        int exit_status = mapper_status == 0 ? 0 : 1;

        _exit(exit_status);
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PROCESS_CREATE", "mapper pid=%ld", (long)mapper_pid);
    }

    if (result == 0) {
        reducer_pid = fork();

        if (reducer_pid == -1) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (result == 0 && reducer_pid == 0) {
        close_if_open(&log_file_fd);
        if (dup2(mapper_to_reducer[0], STDIN_FILENO) == -1) { _exit(1); }
        if (dup2(reducer_to_main[1], STDOUT_FILENO) == -1) { _exit(1); }

        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);
        close_if_open(&mapper_log_to_main[0]);
        close_if_open(&mapper_log_to_main[1]);
        close_if_open(&reducer_log_to_main[0]);

        log_message(reducer_log_to_main[1], NULL, "reducer", "main", "PROCESS_START", "reducer process started");
        int reducer_status = reducer_process_main(mr, reducer_log_to_main[1]);
        log_message(reducer_log_to_main[1], NULL, "reducer", "main", "PROCESS_END", "reducer process ended status=%d",
                    reducer_status);
        close_if_open(&reducer_log_to_main[1]);
        int exit_status = reducer_status == 0 ? 0 : 1;

        _exit(exit_status);
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PROCESS_CREATE", "reducer pid=%ld",
                    (long)reducer_pid);
    }

    if (close_if_open(&mapper_log_to_main[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&reducer_log_to_main[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0) {
        mapper_log_arg.in_fd = mapper_log_to_main[0];
        mapper_log_arg.out_fd = log_file_fd;
        mapper_log_arg.lock = &log_file_lock;
        if (thrd_create(&mapper_log_thread, log_collector_main, &mapper_log_arg) != thrd_success) {
            saved_errno = EIO;
            result = -1;
        } else {
            mapper_log_thread_started = 1;
        }
    }

    if (result == 0) {
        reducer_log_arg.in_fd = reducer_log_to_main[0];
        reducer_log_arg.out_fd = log_file_fd;
        reducer_log_arg.lock = &log_file_lock;
        if (thrd_create(&reducer_log_thread, log_collector_main, &reducer_log_arg) != thrd_success) {
            saved_errno = EIO;
            result = -1;
        } else {
            reducer_log_thread_started = 1;
        }
    }

    if (close_if_open(&main_to_mapper[0]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&mapper_to_reducer[0]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&mapper_to_reducer[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&reducer_to_main[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0 &&
        write_input_path_lines(main_to_mapper[1], input_path, log_file_fd, &log_file_lock, &lines_sent) == -1) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "COUNT", "lines_sent=%zu", lines_sent);
    }

    if (close_if_open(&main_to_mapper[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0 &&
        write_reducer_results(reducer_to_main[0], output_path, log_file_fd, &log_file_lock, &results_written) == -1) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "COUNT", "results_written=%zu", results_written);
    }

    if (close_if_open(&reducer_to_main[0]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == -1) {
        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);
    }

    if (mapper_pid != -1) {
        if (wait_if_started(&mapper_pid, &mapper_status) == -1 && result == 0) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (reducer_pid != -1) {
        if (wait_if_started(&reducer_pid, &reducer_status) == -1 && result == 0) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (result == 0 && (!WIFEXITED(mapper_status) || WEXITSTATUS(mapper_status) != 0 || !WIFEXITED(reducer_status) ||
                        WEXITSTATUS(reducer_status) != 0)) {
        saved_errno = ECHILD;
        result = -1;
    }

    if (mapper_log_thread_started != 0) {
        int status = 0;
        if (thrd_join(mapper_log_thread, &status) != thrd_success && result == 0) {
            saved_errno = EIO;
            result = -1;
        } else if (status != 0 && result == 0) {
            saved_errno = EIO;
            result = -1;
        }
        mapper_log_thread_started = 0;
    }

    if (reducer_log_thread_started != 0) {
        int status = 0;
        if (thrd_join(reducer_log_thread, &status) != thrd_success && result == 0) {
            saved_errno = EIO;
            result = -1;
        } else if (status != 0 && result == 0) {
            saved_errno = EIO;
            result = -1;
        }
        reducer_log_thread_started = 0;
    }

    close_if_open(&main_to_mapper[0]);
    close_if_open(&main_to_mapper[1]);
    close_if_open(&mapper_to_reducer[0]);
    close_if_open(&mapper_to_reducer[1]);
    close_if_open(&reducer_to_main[0]);
    close_if_open(&reducer_to_main[1]);
    close_if_open(&mapper_log_to_main[0]);
    close_if_open(&mapper_log_to_main[1]);
    close_if_open(&reducer_log_to_main[0]);
    close_if_open(&reducer_log_to_main[1]);
    wait_if_started(&mapper_pid, NULL);
    wait_if_started(&reducer_pid, NULL);

    if (log_file_fd != -1) {
        if (result == -1) {
            log_message(log_file_fd, log_file_lock_ready != 0 ? &log_file_lock : NULL, "main", "main", "ERROR",
                        "mr_start failed errno=%d", saved_errno);
        }
        close_if_open(&log_file_fd);
    }

    if (log_file_lock_ready != 0) { mtx_destroy(&log_file_lock); }

    if (result == -1) { errno = saved_errno; }
    return result;
}
