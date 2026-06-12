#define _POSIX_C_SOURCE 200809L

#include "mr.h"
#include <errno.h>
#include <stddef.h>
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

typedef struct {
    const char *file_name;
    const char *line;
} expected_line_t;

typedef struct {
    int token_len;
    int result_len;
} expected_result_header_t;

static int create_text_file(char *path, const char *content) {
    int fd = mkstemp(path);
    if (fd == -1) { return -1; }

    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if (fputs(content, fp) == EOF) {
        int saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    if (fclose(fp) == EOF) { return -1; }

    return 0;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');

    if (slash == NULL) { return path; }
    return slash + 1;
}

static int validating_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                             void *user_arg) {
    expected_line_t *expected = user_arg;
    const int value = 1;

    if (expected == NULL || line == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(line->file_name, expected->file_name) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (line->line_number != 1 || strcmp(line->line, expected->line) != 0) {
        errno = EINVAL;
        return -1;
    }

    return emit("Token", &value, sizeof(value), emit_arg);
}

static int sum_reducer(const char *token, const mr_value_t *values, size_t values_count,
                       mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    if (token == NULL || strcmp(token, "Token") != 0 || values == NULL || values_count != 1) {
        errno = EINVAL;
        return -1;
    }

    if (values[0].data == NULL || values[0].size != sizeof(int)) {
        errno = EINVAL;
        return -1;
    }

    int total = 0;
    memcpy(&total, values[0].data, sizeof(total));

    return emit(token, &total, sizeof(total), emit_arg);
}

static int failing_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                          void *user_arg) {
    (void)line;
    (void)emit;
    (void)emit_arg;
    (void)user_arg;

    errno = EINVAL;
    return -1;
}

static int dummy_reducer(const char *token, const mr_value_t *values, size_t values_count,
                         mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)token;
    (void)values;
    (void)values_count;
    (void)emit;
    (void)emit_arg;
    (void)user_arg;
    return 0;
}

static int expect_output_result(const char *path, const char *token, int expected_total) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) { return 1; }

    int failures = 0;
    expected_result_header_t header;
    char token_buffer[32] = { 0 };
    int actual_total = 0;
    int extra = 0;
    size_t token_len = strlen(token);

    failures += expect_int(fread(&header, sizeof(header), 1, fp) == 1,
                           "output deve contenere un header risultato");
    failures += expect_int(header.token_len == (int)token_len,
                           "output deve contenere la lunghezza del token");
    failures += expect_int(header.result_len == (int)sizeof(actual_total),
                           "output deve contenere la lunghezza del risultato");

    if (failures == 0) {
        failures += expect_int(token_len < sizeof(token_buffer),
                               "buffer token del test deve essere sufficiente");
        failures += expect_int(fread(token_buffer, token_len, 1, fp) == 1,
                               "output deve contenere il token");
        failures += expect_int(memcmp(token_buffer, token, token_len) == 0,
                               "output deve contenere il token atteso");
        failures += expect_int(fread(&actual_total, sizeof(actual_total), 1, fp) == 1,
                               "output deve contenere il totale");
        failures += expect_int(actual_total == expected_total,
                               "output deve contenere il totale atteso");
        failures += expect_int(fread(&extra, 1, 1, fp) == 0,
                               "output non deve contenere byte extra");
    }

    failures += expect_int(fclose(fp) == 0, "chiusura output deve riuscire");
    return failures;
}

int main(void) {
    mr_attr_t attr;
    mr_t mr = NULL;
    char valid_input_path[] = "/tmp/mr-start-valid-XXXXXX";
    char valid_output_path[] = "/tmp/mr-start-output-XXXXXX";
    char failing_input_path[] = "/tmp/mr-start-failing-XXXXXX";
    expected_line_t expected = { 0 };
    int failures = 0;

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(create_text_file(valid_input_path, "alpha\n") == 0,
                           "creazione input valido deve riuscire");
    int output_fd = mkstemp(valid_output_path);
    failures += expect_int(output_fd != -1, "creazione path output deve riuscire");
    if (output_fd != -1) { failures += expect_int(close(output_fd) == 0, "close path output deve riuscire"); }
    failures += expect_int(create_text_file(failing_input_path, "beta\n") == 0,
                           "creazione input fallimento deve riuscire");

    expected.file_name = base_name(valid_input_path);
    expected.line = "alpha";

    failures += expect_int(mr_create(&mr, &attr, validating_mapper, sum_reducer, &expected) == 0,
                           "mr_create deve riuscire");

    errno = 0;
    failures += expect_int(mr_start(NULL, "input.txt", "output.mro") == -1,
                           "mr_start deve rifiutare mr NULL");
    failures += expect_int(errno == EINVAL, "mr_start con mr NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_start(mr, NULL, "output.mro") == -1,
                           "mr_start deve rifiutare input_path NULL");
    failures += expect_int(errno == EINVAL, "mr_start con input_path NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_start(mr, "input.txt", NULL) == -1,
                           "mr_start deve rifiutare output_path NULL");
    failures += expect_int(errno == EINVAL, "mr_start con output_path NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_start(mr, valid_input_path, valid_output_path) == 0,
                           "mr_start deve produrre un risultato valido");
    failures += expect_output_result(valid_output_path, "Token", 1);

    if (mr != NULL) {
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy deve riuscire");
        mr = NULL;
    }

    failures += expect_int(mr_create(&mr, &attr, failing_mapper, dummy_reducer, NULL) == 0,
                           "mr_create con mapper fallente deve riuscire");

    errno = 0;
    failures += expect_int(mr_start(mr, failing_input_path, "output.mro") == -1,
                           "mr_start deve fallire se il processo mapper fallisce");
    failures += expect_int(errno == ECHILD,
                           "mr_start con mapper fallente deve segnalare terminazione non riuscita");

    if (mr != NULL) {
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy finale deve riuscire");
    }

    unlink(valid_input_path);
    unlink(valid_output_path);
    unlink(failing_input_path);

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_start: ok\n");
    return 0;
}
