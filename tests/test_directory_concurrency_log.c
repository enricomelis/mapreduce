#define _POSIX_C_SOURCE 200809L

#include "../src/mr_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int expect_int(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }

    return 0;
}

static int create_empty_path(char *path) {
    int fd = mkstemp(path);
    if (fd == -1) { return -1; }
    return close(fd);
}

static int join_path(char *out, size_t out_size, const char *directory, const char *file_name) {
    int n = snprintf(out, out_size, "%s/%s", directory, file_name);

    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int create_file_in_directory(const char *directory, const char *file_name,
                                    const char *content) {
    char path[512];
    FILE *fp;

    if (join_path(path, sizeof(path), directory, file_name) == -1) { return -1; }

    fp = fopen(path, "w");
    if (fp == NULL) { return -1; }

    if (fputs(content, fp) == EOF) {
        int saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    if (fclose(fp) == EOF) { return -1; }
    return 0;
}

static int directory_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                            void *user_arg) {
    (void)user_arg;

    const char *token = NULL;
    unsigned char payload[4] = {0};

    if (line == NULL || line->file_name == NULL || line->line == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(line->file_name, "a.txt") == 0 && line->line_number == 1 &&
        strcmp(line->line, "apricot") == 0) {
        token = "A1";
        payload[0] = 'a';
        payload[1] = 1;
        payload[2] = (unsigned char)line->line_len;
        payload[3] = 'a';
    } else if (strcmp(line->file_name, "a.txt") == 0 && line->line_number == 2 &&
               strcmp(line->line, "avocado") == 0) {
        token = "A2";
        payload[0] = 'a';
        payload[1] = 2;
        payload[2] = (unsigned char)line->line_len;
        payload[3] = 'v';
    } else if (strcmp(line->file_name, "b.txt") == 0 && line->line_number == 1 &&
               strcmp(line->line, "banana") == 0) {
        token = "B1";
        payload[0] = 'b';
        payload[1] = 1;
        payload[2] = (unsigned char)line->line_len;
        payload[3] = 'b';
    } else if (strcmp(line->file_name, "c.txt") == 0 && line->line_number == 1 &&
               strcmp(line->line, "citrus") == 0) {
        token = "C1";
        payload[0] = 'c';
        payload[1] = 1;
        payload[2] = (unsigned char)line->line_len;
        payload[3] = 'c';
    } else {
        errno = EINVAL;
        return -1;
    }

    return emit(token, payload, sizeof(payload), emit_arg);
}

static int directory_reducer(const char *token, const mr_value_t *values, size_t values_count,
                             mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    unsigned char result[4] = {0};

    if (token == NULL || values == NULL || values_count != 1 || values[0].data == NULL ||
        values[0].size != sizeof(result)) {
        errno = EINVAL;
        return -1;
    }

    memcpy(result, values[0].data, sizeof(result));

    if ((strcmp(token, "A1") == 0 && memcmp(result, (unsigned char[]){'a', 1, 7, 'a'}, 4) == 0) ||
        (strcmp(token, "A2") == 0 && memcmp(result, (unsigned char[]){'a', 2, 7, 'v'}, 4) == 0) ||
        (strcmp(token, "B1") == 0 && memcmp(result, (unsigned char[]){'b', 1, 6, 'b'}, 4) == 0) ||
        (strcmp(token, "C1") == 0 && memcmp(result, (unsigned char[]){'c', 1, 6, 'c'}, 4) == 0)) {
        result[3] = (unsigned char)(result[3] - 32);
        return emit(token, result, sizeof(result), emit_arg);
    }

    errno = EINVAL;
    return -1;
}

static int expect_next_result(FILE *fp, const char *token, const unsigned char *payload,
                              size_t payload_size) {
    mr_result_header_t header;
    char token_buffer[32] = {0};
    unsigned char payload_buffer[32] = {0};
    size_t token_len = strlen(token);
    int failures = 0;

    failures += expect_int(fread(&header, sizeof(header), 1, fp) == 1,
                           "output directory deve contenere header");
    failures += expect_int(header.token_len == (int)token_len,
                           "output directory deve avere token_len atteso");
    failures += expect_int(header.result_len == (int)payload_size,
                           "output directory deve avere result_len atteso");

    if (failures != 0) { return failures; }

    failures += expect_int(fread(token_buffer, token_len, 1, fp) == 1,
                           "output directory deve contenere token");
    failures += expect_int(memcmp(token_buffer, token, token_len) == 0,
                           "output directory deve essere ordinato per token");
    failures += expect_int(fread(payload_buffer, payload_size, 1, fp) == 1,
                           "output directory deve contenere payload");
    failures += expect_int(memcmp(payload_buffer, payload, payload_size) == 0,
                           "payload directory deve essere quello atteso");

    return failures;
}

static int expect_output(const char *path) {
    const unsigned char a1[] = {'a', 1, 7, 'A'};
    const unsigned char a2[] = {'a', 2, 7, 'V'};
    const unsigned char b1[] = {'b', 1, 6, 'B'};
    const unsigned char c1[] = {'c', 1, 6, 'C'};
    FILE *fp = fopen(path, "rb");
    int failures = 0;
    int extra = 0;

    failures += expect_int(fp != NULL, "output directory deve essere apribile");
    if (fp == NULL) { return failures; }

    failures += expect_next_result(fp, "A1", a1, sizeof(a1));
    failures += expect_next_result(fp, "A2", a2, sizeof(a2));
    failures += expect_next_result(fp, "B1", b1, sizeof(b1));
    failures += expect_next_result(fp, "C1", c1, sizeof(c1));
    failures += expect_int(fread(&extra, 1, 1, fp) == 0,
                           "output directory non deve contenere record extra");
    failures += expect_int(fclose(fp) == 0, "output directory deve chiudersi");

    return failures;
}

static int file_contains(const char *path, const char *needle) {
    FILE *fp = fopen(path, "r");
    char line[1024];

    if (fp == NULL) { return 0; }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static void cleanup_directory(const char *directory) {
    char path[512];

    if (join_path(path, sizeof(path), directory, "a.txt") == 0) { unlink(path); }
    if (join_path(path, sizeof(path), directory, "b.txt") == 0) { unlink(path); }
    if (join_path(path, sizeof(path), directory, "c.txt") == 0) { unlink(path); }
    rmdir(directory);
}

int main(void) {
    char directory_path[] = "/tmp/mr-dir-input-XXXXXX";
    char output_path[] = "/tmp/mr-dir-output-XXXXXX";
    char log_path[] = "/tmp/mr-dir-log-XXXXXX";
    mr_attr_t attr;
    mr_t mr = NULL;
    int failures = 0;

    failures += expect_int(mkdtemp(directory_path) != NULL, "mkdtemp directory deve riuscire");
    failures += expect_int(create_file_in_directory(directory_path, "b.txt", "banana\n") == 0,
                           "creazione b.txt deve riuscire");
    failures += expect_int(create_file_in_directory(directory_path, "c.txt", "citrus\n") == 0,
                           "creazione c.txt deve riuscire");
    failures += expect_int(create_file_in_directory(directory_path, "a.txt", "apricot\navocado\n") == 0,
                           "creazione a.txt deve riuscire");
    failures += expect_int(create_empty_path(output_path) == 0,
                           "creazione output directory deve riuscire");
    failures += expect_int(create_empty_path(log_path) == 0, "creazione log directory deve riuscire");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init directory deve riuscire");
    failures += expect_int(mr_attr_set_mapper_threads(&attr, 3) == 0,
                           "mapper_threads multipli devono essere accettati");
    failures += expect_int(mr_attr_set_reducer_threads(&attr, 2) == 0,
                           "reducer_threads multipli devono essere accettati");
    failures += expect_int(mr_attr_set_queue_size(&attr, 2) == 0,
                           "queue_size piccolo deve essere accettato");
    failures += expect_int(mr_attr_set_log_file(&attr, log_path) == 0,
                           "log_file directory deve essere accettato");
    failures += expect_int(mr_create(&mr, &attr, directory_mapper, directory_reducer, NULL) == 0,
                           "mr_create directory deve riuscire");

    if (mr != NULL) {
        failures += expect_int(mr_start(mr, directory_path, output_path) == 0,
                               "mr_start directory deve riuscire");
        failures += expect_output(output_path);
        failures += expect_int(file_contains(log_path, "PIPE_CREATE"),
                               "log deve contenere eventi pipe");
        failures += expect_int(file_contains(log_path, "PROCESS_CREATE"),
                               "log deve contenere eventi process");
        failures += expect_int(file_contains(log_path, "THREAD_START"),
                               "log deve contenere eventi thread");
        failures += expect_int(file_contains(log_path, "COUNT"),
                               "log deve contenere eventi count");
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy directory deve riuscire");
    }

    cleanup_directory(directory_path);
    unlink(output_path);
    unlink(log_path);

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_directory_concurrency_log: ok\n");
    return 0;
}
