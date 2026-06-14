#include "mr_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int write_line_record(int fd, const mr_line_item_t *item) {
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

int read_line_record(int fd, mr_line_item_t *out) {
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

int is_valid_token(const char *token) {
    if (token == NULL || token[0] == '\0') { return 0; }

    for (const char *p = token; *p != '\0'; p++) {
        int is_digit = *p >= '0' && *p <= '9';
        int is_upper = *p >= 'A' && *p <= 'Z';
        int is_lower = *p >= 'a' && *p <= 'z';

        if (!is_digit && !is_upper && !is_lower) { return 0; }
    }

    return 1;
}

void pair_item_destroy(mr_pair_item_t *item) {
    if (item == NULL) { return; }

    free(item->token);
    free(item->value);

    *item = (mr_pair_item_t){ 0 };

    return;
}

int read_pair_record(int fd, mr_pair_item_t *item_out) {
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

int write_reducer_results(int in_fd, const char *output_path, int log_fd, mtx_t *log_lock, size_t *results_written) {
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

void result_list_destroy(mr_result_list_t *list) {
    if (list == NULL) { return; }

    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].token);
        free(list->items[i].data);
    }

    free(list->items);
    *list = (mr_result_list_t){ 0 };
}

int result_list_push(mr_result_list_t *list, const char *token, const void *result, size_t result_size) {
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

int reducer_collect_result(const char *token, const void *result, size_t result_size, void *emit_arg) {
    return result_list_push(emit_arg, token, result, result_size);
}

int write_result_list(int fd, const mr_result_list_t *list) {
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
