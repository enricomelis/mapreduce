#include "mr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_COUNT_MAPPER_THREADS  2
#define WORD_COUNT_REDUCER_THREADS 2
#define WORD_COUNT_QUEUE_SIZE      64

static int is_token_char(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static int word_count_mapper(const mr_file_line_t *file_line, mr_emit_pair_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    if (file_line == NULL || emit == NULL) {
        errno = EINVAL;
        return -1;
    }

    char *token = malloc(file_line->line_len + 1);
    if (token == NULL) { return -1; }

    int one = 1;
    size_t i = 0;

    while (i < file_line->line_len) {
        while (i < file_line->line_len && !is_token_char(file_line->line[i])) {
            i++;
        }

        size_t start = i;
        while (i < file_line->line_len && is_token_char(file_line->line[i])) {
            i++;
        }

        size_t token_len = i - start;
        if (token_len > 0) {
            memcpy(token, file_line->line + start, token_len);
            token[token_len] = '\0';

            if (emit(token, &one, sizeof(one), emit_arg) == -1) {
                int saved_errno = errno;
                free(token);
                errno = saved_errno;
                return -1;
            }
        }
    }

    free(token);
    return 0;
}

static int word_count_reducer(const char *token, const mr_value_t *values, size_t values_count, mr_emit_result_t emit,
                              void *emit_arg, void *user_arg) {
    (void)user_arg;

    if (token == NULL || values == NULL || emit == NULL) {
        errno = EINVAL;
        return -1;
    }

    int total = 0;

    for (size_t i = 0; i < values_count; i++) {
        if (values[i].data == NULL || values[i].size != sizeof(int)) {
            errno = EINVAL;
            return -1;
        }

        int value = 0;
        memcpy(&value, values[i].data, sizeof(value));
        total += value;
    }

    return emit(token, &total, sizeof(total), emit_arg);
}

static int run_word_count(const char *input_path, const char *output_path, const char *log_path) {
    mr_attr_t attr;
    mr_t mr = NULL;

    if (mr_attr_init(&attr) == -1) { return -1; }

    if (mr_attr_set_mapper_threads(&attr, WORD_COUNT_MAPPER_THREADS) == -1 ||
        mr_attr_set_reducer_threads(&attr, WORD_COUNT_REDUCER_THREADS) == -1 ||
        mr_attr_set_queue_size(&attr, WORD_COUNT_QUEUE_SIZE) == -1) {
        mr_attr_destroy(&attr);
        return -1;
    }

    if (log_path != NULL && mr_attr_set_log_file(&attr, log_path) == -1) {
        mr_attr_destroy(&attr);
        return -1;
    }

    if (mr_create(&mr, &attr, word_count_mapper, word_count_reducer, NULL) == -1) {
        mr_attr_destroy(&attr);
        return -1;
    }

    int result = mr_start(mr, input_path, output_path);
    int saved_errno = errno;

    if (mr_destroy(mr) == -1 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    if (mr_attr_destroy(&attr) == -1 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    if (result == -1) { errno = saved_errno; }
    return result;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s INPUT OUTPUT [LOG]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *log_path = argc == 4 ? argv[3] : NULL;

    if (run_word_count(argv[1], argv[2], log_path) == -1) {
        perror("word_count");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
