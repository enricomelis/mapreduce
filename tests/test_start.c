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

int main(void) {
    mr_attr_t attr;
    mr_t mr = NULL;
    char valid_input_path[] = "/tmp/mr-start-valid-XXXXXX";
    char failing_input_path[] = "/tmp/mr-start-failing-XXXXXX";
    expected_line_t expected = { 0 };
    int failures = 0;

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(create_text_file(valid_input_path, "alpha\n") == 0,
                           "creazione input valido deve riuscire");
    failures += expect_int(create_text_file(failing_input_path, "beta\n") == 0,
                           "creazione input fallimento deve riuscire");

    expected.file_name = base_name(valid_input_path);
    expected.line = "alpha";

    failures += expect_int(mr_create(&mr, &attr, validating_mapper, dummy_reducer, &expected) == 0,
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
    failures += expect_int(mr_start(mr, valid_input_path, "output.mro") == 0,
                           "mr_start deve avviare il mapper e processare una riga valida");

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
    unlink(failing_input_path);

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_start: ok\n");
    return 0;
}
