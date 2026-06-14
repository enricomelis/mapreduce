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

static int create_empty_path(char *path) {
    int fd = mkstemp(path);
    if (fd == -1) { return -1; }
    return close(fd);
}

static int has_value(const mr_value_t *values, size_t values_count, const unsigned char *data,
                     size_t size) {
    for (size_t i = 0; i < values_count; i++) {
        if (values[i].size == size &&
            (size == 0 || (values[i].data != NULL && memcmp(values[i].data, data, size) == 0))) {
            return 1;
        }
    }

    return 0;
}

static int generic_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                          void *user_arg) {
    (void)user_arg;

    if (line == NULL || line->file_name == NULL || line->line == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (line->line_number == 1 && strcmp(line->line, "north") == 0) {
        const unsigned char alpha[] = {0x01, 0xa0};
        const unsigned char beta[] = {0xb1};

        if (emit("Alpha", alpha, sizeof(alpha), emit_arg) == -1) { return -1; }
        return emit("Beta", beta, sizeof(beta), emit_arg);
    }

    if (line->line_number == 2 && strcmp(line->line, "south") == 0) {
        const unsigned char alpha[] = {0x02, 0xa1};

        if (emit("Alpha", alpha, sizeof(alpha), emit_arg) == -1) { return -1; }
        return emit("Gamma", NULL, 0, emit_arg);
    }

    errno = EINVAL;
    return -1;
}

static int generic_reducer(const char *token, const mr_value_t *values, size_t values_count,
                           mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    if (token == NULL || values == NULL || values_count == 0) {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(token, "Alpha") == 0) {
        const unsigned char first[] = {0x01, 0xa0};
        const unsigned char second[] = {0x02, 0xa1};
        const unsigned char result[] = {0xa3, 0x02, 0x01};

        if (values_count != 2 || !has_value(values, values_count, first, sizeof(first)) ||
            !has_value(values, values_count, second, sizeof(second))) {
            errno = EINVAL;
            return -1;
        }

        return emit(token, result, sizeof(result), emit_arg);
    }

    if (strcmp(token, "Beta") == 0) {
        const unsigned char value[] = {0xb1};
        const unsigned char result[] = {0xb1, 0x01};

        if (values_count != 1 || !has_value(values, values_count, value, sizeof(value))) {
            errno = EINVAL;
            return -1;
        }

        return emit(token, result, sizeof(result), emit_arg);
    }

    if (strcmp(token, "Gamma") == 0) {
        if (values_count != 1 || values[0].size != 0 || values[0].data != NULL) {
            errno = EINVAL;
            return -1;
        }

        return emit(token, NULL, 0, emit_arg);
    }

    errno = EINVAL;
    return -1;
}

static int expect_next_result(FILE *fp, const char *token, const unsigned char *payload,
                              size_t payload_size) {
    mr_result_header_t header;
    char token_buffer[64] = {0};
    unsigned char payload_buffer[64] = {0};
    size_t token_len = strlen(token);
    int failures = 0;

    failures += expect_int(fread(&header, sizeof(header), 1, fp) == 1,
                           "output deve contenere un header risultato");
    failures += expect_int(header.token_len == (int)token_len,
                           "header risultato deve contenere token_len atteso");
    failures += expect_int(header.result_len == (int)payload_size,
                           "header risultato deve contenere result_len atteso");

    if (failures != 0) { return failures; }

    failures += expect_int(token_len < sizeof(token_buffer), "buffer token test deve bastare");
    failures += expect_int(payload_size <= sizeof(payload_buffer), "buffer payload test deve bastare");
    failures += expect_int(fread(token_buffer, token_len, 1, fp) == 1,
                           "output deve contenere token risultato");
    failures += expect_int(memcmp(token_buffer, token, token_len) == 0,
                           "output deve contenere token atteso");

    if (payload_size > 0) {
        failures += expect_int(fread(payload_buffer, payload_size, 1, fp) == 1,
                               "output deve contenere payload risultato");
        failures += expect_int(memcmp(payload_buffer, payload, payload_size) == 0,
                               "payload risultato deve essere quello atteso");
    }

    return failures;
}

static int expect_output(const char *path) {
    const unsigned char alpha[] = {0xa3, 0x02, 0x01};
    const unsigned char beta[] = {0xb1, 0x01};
    int failures = 0;
    int extra = 0;
    FILE *fp = fopen(path, "rb");

    failures += expect_int(fp != NULL, "output deve essere apribile");
    if (fp == NULL) { return failures; }

    failures += expect_next_result(fp, "Alpha", alpha, sizeof(alpha));
    failures += expect_next_result(fp, "Beta", beta, sizeof(beta));
    failures += expect_next_result(fp, "Gamma", NULL, 0);
    failures += expect_int(fread(&extra, 1, 1, fp) == 0,
                           "output non deve contenere record extra");
    failures += expect_int(fclose(fp) == 0, "output deve chiudersi correttamente");

    return failures;
}

int main(void) {
    char input_path[] = "/tmp/mr-generic-input-XXXXXX";
    char output_path[] = "/tmp/mr-generic-output-XXXXXX";
    char log_path[] = "/tmp/mr-generic-log-XXXXXX";
    mr_attr_t attr;
    mr_t mr = NULL;
    int failures = 0;

    failures += expect_int(create_text_file(input_path, "north\nsouth\n") == 0,
                           "creazione input generico deve riuscire");
    failures += expect_int(create_empty_path(output_path) == 0,
                           "creazione output generico deve riuscire");
    failures += expect_int(create_empty_path(log_path) == 0,
                           "creazione log generico deve riuscire");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(mr_attr_set_log_file(&attr, log_path) == 0,
                           "log_file custom deve essere accettato");
    failures += expect_int(mr_create(&mr, &attr, generic_mapper, generic_reducer, NULL) == 0,
                           "mr_create generico deve riuscire");

    if (mr != NULL) {
        failures += expect_int(mr_start(mr, input_path, output_path) == 0,
                               "mr_start generico deve riuscire");
        failures += expect_output(output_path);
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy generico deve riuscire");
    }

    unlink(input_path);
    unlink(output_path);
    unlink(log_path);

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_generic_e2e: ok\n");
    return 0;
}
