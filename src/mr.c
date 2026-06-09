#define _POSIX_C_SOURCE 200809L

#include "mr.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <threads.h>
#include <unistd.h>

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
    char *full_path;
    char *file_name;
} mr_input_file_t;

static int write_input_path_lines(int out_fd, const char *input_path);
static int mapper_process_main(mr_t mr);

static void line_item_destroy(mr_line_item_t *item) {
    if (item == NULL) { return; }

    free((void *)item->file_name);
    free((void *)item->line);
    *item = (mr_line_item_t){ 0 };

    return;
}

/* ====================================================================== */
/* coda per pattern produttore-consumatore nei thread del processo mapper */
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

/*
 * inizializzazione della coda:
 * 1. controllo input invalidi
 * 2. allocazione memoria dinamica inizializzata
 * 3. campi della struct
 * 4. strutture di sincronizzazione con eventuale distruzione dei precedenti
 */
static int line_queue_init(mr_line_queue_t *queue, size_t capacity) {
    if (capacity == 0 || queue == NULL) {
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

/* distruzione della coda
 * 1. controllo input invalidi
 * 2. free circolare sugli elementi validi della coda
 * 3. distruzione delle strutture e azzeramento dei campi
 */
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
    if (queue == NULL || item == NULL) {
        errno = EINVAL;
        return -1;
    }

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

/* valori di return
 * `-1`: errore
 * `1`: item estratto
 * `0`: coda chiusa e vuota
 */
static int line_queue_pop(mr_line_queue_t *queue, mr_line_item_t *item) {
    if (queue == NULL || item == NULL) {
        errno = EINVAL;
        return -1;
    }

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

/* ====================================================================== */
/* gestione di mr_attr_** */

int mr_attr_init(mr_attr_t *attr) {
    if (attr == NULL) {
        errno = EINVAL;
        return -1;
    }

    attr->mapper_threads = 1;
    attr->reducer_threads = 1;
    attr->queue_size = 64;
    attr->log_file = NULL;

    return 0;
}

int mr_attr_destroy(mr_attr_t *attr) {
    if (attr == NULL) {
        errno = EINVAL;
        return -1;
    }

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

    attr->log_file = path;
    return 0;
}

/* ====================================================================== */
/* gestione di mr_create, mr_destroy e mr_start */

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

int mr_start(mr_t mr, const char *input_path, const char *output_path) {
    MR_CHECK_NULL(mr);
    MR_CHECK_NULL(input_path);
    MR_CHECK_NULL(output_path);

    int main_to_mapper[2] = { -1, -1 };
    int mapper_to_main[2] = { -1, -1 }; /* temporanea: sostituisce la futura pipe mapper -> reducer */

    if (pipe(main_to_mapper) == -1) { return -1; }

    if (pipe(mapper_to_main) == -1) {
        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        return -1;
    }

    pid_t mapper_pid = fork();

    if (mapper_pid == -1) {
        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        close(mapper_to_main[0]);
        close(mapper_to_main[1]);

        return -1;
    }

    if (mapper_pid == 0) {
        if (dup2(main_to_mapper[0], STDIN_FILENO) == -1) { _exit(1); }
        if (dup2(mapper_to_main[1], STDOUT_FILENO) == -1) { _exit(1); }

        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        close(mapper_to_main[0]);
        close(mapper_to_main[1]);

        int mapper_status = mapper_process_main(mr);
        int exit_status = mapper_status == 0 ? 0 : 1;

        _exit(exit_status);
    }

    int result = 0;

    if (close(main_to_mapper[0]) == -1) { result = -1; }
    main_to_mapper[0] = -1;

    if (close(mapper_to_main[1]) == -1) { result = -1; }
    mapper_to_main[1] = -1;

    if (write_input_path_lines(main_to_mapper[1], input_path) == -1) { result = -1; }

    if (close(main_to_mapper[1]) == -1) { result = -1; }
    main_to_mapper[1] = -1;

    /* Inizio drain temporaneo: finche manca il reducer, il padre svuota l'output del mapper. */
    char drain_buffer[4096];
    while (1) {
        ssize_t n_read = read(mapper_to_main[0], drain_buffer, sizeof(drain_buffer));

        if (n_read > 0) { continue; }
        if (n_read == 0) { break; }
        if (errno == EINTR) { continue; }

        result = -1;
        break;
    }
    /* Fine drain temporaneo. */

    if (close(mapper_to_main[0]) == -1) { result = -1; }
    mapper_to_main[0] = -1;

    int status;
    if (waitpid(mapper_pid, &status, 0) == -1) { return -1; }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = ECHILD;
        return -1;
    }

    return result;
}

/* ====================================================================== */
/* funzioni readn() e writen() */

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

/* ====================================================================== */
/* gestione pipe */

/* valori di return
 * `-1`: errore
 * `0`: record scritto
 */
static int write_line_record(int fd, const mr_line_item_t *item) {
    mr_line_header_t header;

    if (item == NULL) {
        errno = EINVAL;
        return -1;
    }

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

/* valori di return
 * `-1`: errore
 * `0`: EOF pulito
 * `1`: record letto
 */
static int read_line_record(int fd, mr_line_item_t *out) {
    mr_line_header_t header;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

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

static int write_file_lines(int out_fd, const char *path, const char *file_name) {
    if (path == NULL || file_name == NULL) {
        errno = EINVAL;
        return -1;
    }

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

    return 0;
}

/* ====================================================================== */
/* funzioni helper per lettura input */

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
    if (directory == NULL || file_name == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

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
    if (files == NULL || count == NULL || capacity == NULL || full_path == NULL || file_name == NULL) {
        errno = EINVAL;
        return -1;
    }

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

/* DIR: creazione array di files e ciclo di lettura */
static int write_directory_lines(int out_fd, const char *input_path) {
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
        if (write_file_lines(out_fd, files[i].full_path, files[i].file_name) == -1) {
            int saved_errno = errno;
            input_files_destroy(files, count);
            errno = saved_errno;
            return -1;
        }
    }

    input_files_destroy(files, count);
    return 0;
}

/* ====================================================================== */
/* funzione discriminante per lettura input */

static int write_input_path_lines(int out_fd, const char *input_path) {
    if (input_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (stat(input_path, &st) == -1) { return -1; }

    if (S_ISREG(st.st_mode)) { return write_file_lines(out_fd, input_path, input_path_basename(input_path)); }

    if (S_ISDIR(st.st_mode)) { return write_directory_lines(out_fd, input_path); }

    errno = EINVAL;
    return -1;
}

/* ====================================================================== */
/* gestione del mapper */

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
    mtx_t pipe_emit_lock; /* protezione dei record sulla pipe durante l'emit */
} mr_mapper_context_t;

static int mapper_emit_pair(const char *token, const void *value, size_t value_size, void *emit_arg) {
    if (emit_arg == NULL) {
        errno = EINVAL;
        return -1;
    }
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

    mtx_unlock(&context->pipe_emit_lock);

    return 0;
}

static int mapper_reader_main(void *arg) {
    if (arg == NULL) {
        errno = EINVAL;
        return -1;
    }
    mr_mapper_context_t *context = arg;

    mr_line_item_t item = { 0 };

    while (1) {
        item = (mr_line_item_t){ 0 };
        int queue_status = read_line_record(STDIN_FILENO, &item);

        if (queue_status == -1) {
            line_queue_close(&context->queue);
            return -1;
        }

        if (queue_status == 0) {
            line_queue_close(&context->queue);
            return 0;
        }

        if (queue_status == 1) {
            if (line_queue_push(&context->queue, &item) == -1) {
                line_item_destroy(&item);
                line_queue_close(&context->queue);
                return -1;
            }
        }
    }
    return 0;
}

static int mapper_worker_main(void *arg) {
    if (arg == NULL) {
        errno = EINVAL;
        return -1;
    }
    mr_mapper_context_t *context = arg;

    while (1) {
        mr_line_item_t item = { 0 };
        int queue_status = line_queue_pop(&context->queue, &item);

        if (queue_status == -1) {
            line_queue_close(&context->queue);
            return -1;
        }

        if (queue_status == 0) { return 0; }

        mr_file_line_t line = { .file_name = item.file_name,
                                .file_name_len = item.file_name_len,
                                .line = item.line,
                                .line_len = item.line_len,
                                .line_number = item.line_number };

        int mapper_status = context->mapper(&line, mapper_emit_pair, context, context->user_arg);

        line_item_destroy(&item);

        if (mapper_status == -1) {
            line_queue_close(&context->queue);
            return -1;
        }
    }

    return 0;
}

static int mapper_process_main(mr_t mr) {
    MR_CHECK_NULL(mr);

    mr_mapper_context_t context = { 0 };
    context.mapper = mr->mapper;
    context.user_arg = mr->user_arg;
    context.out_fd = STDOUT_FILENO;

    if (line_queue_init(&context.queue, mr->attr.queue_size) == -1) { return -1; }

    if (mtx_init(&context.pipe_emit_lock, mtx_plain) != thrd_success) {
        line_queue_destroy(&context.queue);
        return -1;
    }

    thrd_t *worker_threads = malloc(mr->attr.mapper_threads * sizeof(*worker_threads));
    if (worker_threads == NULL) {
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    size_t workers_created = 0;
    int result = 0;

    for (size_t i = 0; i < mr->attr.mapper_threads; i++) {
        if (thrd_create(&worker_threads[i], mapper_worker_main, &context) != thrd_success) {
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

        free(worker_threads);
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

        free(worker_threads);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    if (thrd_join(reader_thread, &status) != thrd_success || status != 0) { result = -1; }

    for (size_t i = 0; i < workers_created; i++) {
        if (thrd_join(worker_threads[i], &status) != thrd_success || status != 0) { result = -1; }
    }

    free(worker_threads);
    mtx_destroy(&context.pipe_emit_lock);
    line_queue_destroy(&context.queue);

    return result;
}

/* ====================================================================== */
/* gestione del reducer */

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

/* valori di return
 * `-1`: errore
 * `0`: EOF pulito
 * `1`: coppia letta
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

typedef struct {
    char *token;
    size_t token_len;
    mr_value_t *values;
    size_t values_count;
    size_t values_capacity;
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

/* static int pair_groups_add_pair(mr_pair_groups_t *groups, mr_pair_item_t *pair); */
