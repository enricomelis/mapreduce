#ifndef MR_INTERNAL_H
#define MR_INTERNAL_H

#define _POSIX_C_SOURCE 200809L

#include "mr.h"
#include <stddef.h>
#include <sys/types.h>
#include <threads.h>

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
    int in_fd;
    int out_fd;
    mtx_t *lock;
} mr_log_collector_arg_t;

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

typedef struct {
    char *token;
    size_t token_len;
    void *value;
    size_t value_len;
} mr_pair_item_t;

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

ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);
int close_if_open(int *fd);
int wait_if_started(pid_t *pid, int *status);

void line_item_destroy(mr_line_item_t *item);
int line_queue_init(mr_line_queue_t *queue, size_t capacity);
void line_queue_destroy(mr_line_queue_t *queue);
int line_queue_push(mr_line_queue_t *queue, mr_line_item_t *item);
int line_queue_pop(mr_line_queue_t *queue, mr_line_item_t *item);
void line_queue_close(mr_line_queue_t *queue);

int log_message(int fd, mtx_t *lock, const char *process, const char *thread, const char *event, const char *fmt, ...);
int log_collector_main(void *arg);

int write_line_record(int fd, const mr_line_item_t *item);
int read_line_record(int fd, mr_line_item_t *out);
int is_valid_token(const char *token);
void pair_item_destroy(mr_pair_item_t *item);
int read_pair_record(int fd, mr_pair_item_t *item_out);
void result_list_destroy(mr_result_list_t *list);
int result_list_push(mr_result_list_t *list, const char *token, const void *result, size_t result_size);
int reducer_collect_result(const char *token, const void *result, size_t result_size, void *emit_arg);
int write_result_list(int fd, const mr_result_list_t *list);
int write_reducer_results(int in_fd, const char *output_path, int log_fd, mtx_t *log_lock, size_t *results_written);

int write_file_lines(int out_fd, const char *path, const char *file_name, int log_fd, mtx_t *log_lock,
                     size_t *lines_sent);
int write_input_path_lines(int out_fd, const char *input_path, int log_fd, mtx_t *log_lock, size_t *lines_sent);

int mapper_emit_pair(const char *token, const void *value, size_t value_size, void *emit_arg);
int mapper_worker_main(void *arg);
int mapper_process_main(mr_t mr, int log_fd);

void pair_group_destroy(mr_pair_group_t *group);
void pair_groups_destroy(mr_pair_groups_t *groups);
mr_pair_group_t *pair_groups_find(mr_pair_groups_t *groups, const char *token, size_t token_len);
int pair_group_compare(const void *left, const void *right);
mr_pair_group_t *pair_groups_push_group(mr_pair_groups_t *groups, mr_pair_item_t *item);
int pair_group_add_value(mr_pair_group_t *group, mr_pair_item_t *item);
int pair_groups_add_pair(mr_pair_groups_t *groups, mr_pair_item_t *pair);
int collect_pair_groups(int fd, mr_pair_groups_t *groups);
int reducer_process_main(mr_t mr, int log_fd);

#endif
