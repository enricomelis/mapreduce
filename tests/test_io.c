#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/mr_internal.h"

static int expect_int(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }

    return 0;
}

static int close_pair(int pipefd[2]) {
    int result = 0;

    if (pipefd[0] != -1 && close(pipefd[0]) == -1) {
        result = -1;
    }

    if (pipefd[1] != -1 && close(pipefd[1]) == -1) {
        result = -1;
    }

    return result;
}

static void free_line_item(mr_line_item_t *item) {
    if (item == NULL) {
        return;
    }

    free((void *)item->file_name);
    free((void *)item->line);
    *item = (mr_line_item_t){0};
}

static int expect_line_record(int fd, const char *file_name, unsigned long line_number,
                              const char *line) {
    mr_line_item_t item = {0};
    int failures = 0;
    int status = read_line_record(fd, &item);

    if (status != 1) {
        return expect_int(0, "write_file_lines deve produrre il record atteso");
    }

    failures += expect_int(strcmp(item.file_name, file_name) == 0,
                           "write_file_lines deve preservare il nome file logico");
    failures += expect_int(item.line_number == line_number,
                           "write_file_lines deve numerare correttamente le righe");
    failures += expect_int(item.line_len == strlen(line),
                           "write_file_lines deve calcolare correttamente line_len");
    failures += expect_int(strcmp(item.line, line) == 0,
                           "write_file_lines deve scrivere la riga senza newline finale");

    free_line_item(&item);
    return failures;
}

static int create_text_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
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

static int join_path(char *out, size_t out_size, const char *directory, const char *file_name) {
    int n = snprintf(out, out_size, "%s/%s", directory, file_name);

    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int expect_pair_record(int fd, const char *token, const void *value, size_t value_size) {
    mr_pair_header_t header;
    char token_buffer[64] = {0};
    unsigned char value_buffer[64] = {0};
    int failures = 0;
    size_t token_len = strlen(token);

    if (token_len >= sizeof(token_buffer) || value_size > sizeof(value_buffer)) {
        errno = EINVAL;
        return expect_int(0, "expect_pair_record supporta solo payload piccoli");
    }

    failures += expect_int(readn(fd, &header, sizeof(header)) == (ssize_t)sizeof(header),
                           "lettura header coppia deve riuscire");
    failures += expect_int(header.token_len == (int)token_len,
                           "header coppia deve contenere la lunghezza del token");
    failures += expect_int(header.value_len == (int)value_size,
                           "header coppia deve contenere la lunghezza del valore");

    failures += expect_int(readn(fd, token_buffer, token_len) == (ssize_t)token_len,
                           "lettura token coppia deve riuscire");
    failures += expect_int(memcmp(token_buffer, token, token_len) == 0,
                           "token coppia deve essere preservato");

    if (value_size > 0) {
        failures += expect_int(readn(fd, value_buffer, value_size) == (ssize_t)value_size,
                               "lettura valore coppia deve riuscire");
        failures += expect_int(memcmp(value_buffer, value, value_size) == 0,
                               "valore opaco coppia deve essere preservato");
    }

    return failures;
}

static int expect_result_record(int fd, const char *token, const void *result, size_t result_size) {
    mr_result_header_t header;
    char token_buffer[64] = {0};
    unsigned char result_buffer[64] = {0};
    int failures = 0;
    size_t token_len = strlen(token);

    if (token_len >= sizeof(token_buffer) || result_size > sizeof(result_buffer)) {
        errno = EINVAL;
        return expect_int(0, "expect_result_record supporta solo payload piccoli");
    }

    failures += expect_int(readn(fd, &header, sizeof(header)) == (ssize_t)sizeof(header),
                           "lettura header risultato deve riuscire");
    failures += expect_int(header.token_len == (int)token_len,
                           "header risultato deve contenere la lunghezza del token");
    failures += expect_int(header.result_len == (int)result_size,
                           "header risultato deve contenere la lunghezza del risultato");

    failures += expect_int(readn(fd, token_buffer, token_len) == (ssize_t)token_len,
                           "lettura token risultato deve riuscire");
    failures += expect_int(memcmp(token_buffer, token, token_len) == 0,
                           "token risultato deve essere preservato");

    if (result_size > 0) {
        failures += expect_int(readn(fd, result_buffer, result_size) == (ssize_t)result_size,
                               "lettura risultato deve riuscire");
        failures += expect_int(memcmp(result_buffer, result, result_size) == 0,
                               "risultato opaco deve essere preservato");
    }

    return failures;
}

static int expect_pair_item(const mr_pair_item_t *item, const char *token, const void *value,
                            size_t value_size) {
    int failures = 0;
    size_t token_len = strlen(token);

    failures += expect_int(item != NULL, "item coppia letto non deve essere NULL");
    if (item == NULL) { return failures; }

    failures += expect_int(item->token_len == token_len,
                           "read_pair_record deve preservare la lunghezza del token");
    failures += expect_int(strcmp(item->token, token) == 0,
                           "read_pair_record deve ricostruire il token come stringa C");
    failures += expect_int(item->value_len == value_size,
                           "read_pair_record deve preservare la lunghezza del valore");

    if (value_size == 0) {
        failures += expect_int(item->value == NULL,
                               "read_pair_record deve lasciare NULL il valore di dimensione zero");
    } else {
        failures += expect_int(item->value != NULL,
                               "read_pair_record deve allocare il valore non vuoto");
        if (item->value != NULL) {
            failures += expect_int(memcmp(item->value, value, value_size) == 0,
                                   "read_pair_record deve preservare i byte opachi del valore");
        }
    }

    return failures;
}

static int make_pair_item(mr_pair_item_t *item, const char *token, const void *value,
                          size_t value_size) {
    if (item == NULL || token == NULL || (value_size > 0 && value == NULL)) {
        errno = EINVAL;
        return -1;
    }

    *item = (mr_pair_item_t){0};

    item->token = strdup(token);
    if (item->token == NULL) { return -1; }

    item->token_len = strlen(token);

    if (value_size > 0) {
        item->value = malloc(value_size);
        if (item->value == NULL) {
            pair_item_destroy(item);
            return -1;
        }

        memcpy(item->value, value, value_size);
    }

    item->value_len = value_size;

    return 0;
}

static int test_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                       void *user_arg) {
    (void)user_arg;

    if (strcmp(line->file_name, "worker.txt") != 0) {
        errno = EINVAL;
        return -1;
    }

    if (line->line_number != 3 || line->line_len == 0) {
        errno = EINVAL;
        return -1;
    }

    const unsigned char value[] = {line->line[0], 0, (unsigned char)line->line_len};

    return emit("WorkerToken", value, sizeof(value), emit_arg);
}

static int sum_reducer(const char *token, const mr_value_t *values, size_t values_count,
                       mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    int total = 0;

    for (size_t i = 0; i < values_count; i++) {
        if (values[i].size != sizeof(int) || values[i].data == NULL) {
            errno = EINVAL;
            return -1;
        }

        int value;
        memcpy(&value, values[i].data, sizeof(value));
        total += value;
    }

    return emit(token, &total, sizeof(total), emit_arg);
}

int main(void) {
    int pipefd[2] = {-1, -1};
    char input_path[] = "/tmp/mr-test-XXXXXX";
    char directory_path[] = "/tmp/mr-dir-test-XXXXXX";
    char single_input_path[] = "/tmp/mr-single-test-XXXXXX";
    const char message[] = "record-binario";
    const char input_content[] = "alpha\n\nbeta\ngamma";
    char buffer[sizeof(message)] = {0};
    char partial[8] = {0};
    mr_line_item_t line_item = {0};
    mr_line_item_t line_out = {0};
    mr_line_item_t queued_item = {0};
    mr_line_item_t popped_item = {0};
    mr_line_item_t rejected_item = {0};
    mr_pair_item_t pair_item = {0};
    mr_line_queue_t queue = {0};
    mr_line_queue_t closed_queue = {0};
    mr_mapper_context_t mapper_context = {0};
    int input_fd = -1;
    int single_input_fd = -1;
    int emit_lock_ready = 0;
    int worker_queue_ready = 0;
    int worker_lock_ready = 0;
    int failures = 0;

    failures += expect_int(pipe(pipefd) == 0, "pipe deve riuscire");
    if (pipefd[0] == -1 || pipefd[1] == -1) {
        return 1;
    }

    failures += expect_int(writen(pipefd[1], message, sizeof(message)) == (ssize_t)sizeof(message),
                           "writen deve scrivere tutti i byte richiesti");
    failures += expect_int(readn(pipefd[0], buffer, sizeof(buffer)) == (ssize_t)sizeof(buffer),
                           "readn deve leggere tutti i byte richiesti");
    failures += expect_int(memcmp(buffer, message, sizeof(message)) == 0,
                           "i byte letti devono coincidere con quelli scritti");
    failures += expect_int(close_pair(pipefd) == 0, "close della prima pipe deve riuscire");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "seconda pipe deve riuscire");
    failures += expect_int(close(pipefd[1]) == 0, "chiusura lato scrittura deve riuscire");
    pipefd[1] = -1;
    errno = 0;
    failures += expect_int(readn(pipefd[0], partial, sizeof(partial)) == 0,
                           "readn deve restituire 0 su EOF immediato");
    failures += expect_int(close_pair(pipefd) == 0, "close della seconda pipe deve riuscire");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "terza pipe deve riuscire");
    failures += expect_int(writen(pipefd[1], "abc", 3) == 3,
                           "writen deve scrivere il record parziale");
    failures += expect_int(close(pipefd[1]) == 0, "chiusura lato scrittura parziale deve riuscire");
    pipefd[1] = -1;
    errno = 0;
    failures += expect_int(readn(pipefd[0], partial, sizeof(partial)) == -1,
                           "readn deve fallire su EOF a meta record");
    failures += expect_int(errno == EPROTO, "readn su record troncato deve impostare EPROTO");
    failures += expect_int(close_pair(pipefd) == 0, "close della terza pipe deve riuscire");

    errno = 0;
    failures += expect_int(writen(-1, message, sizeof(message)) == -1,
                           "writen deve fallire con fd non valido");
    failures += expect_int(errno == EBADF, "writen con fd non valido deve impostare EBADF");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe round-trip riga deve riuscire");
    line_item.file_name = "input.txt";
    line_item.file_name_len = strlen(line_item.file_name);
    line_item.line_number = 7;
    line_item.line = "alpha beta";
    line_item.line_len = strlen(line_item.line);
    failures += expect_int(write_line_record(pipefd[1], &line_item) == 0,
                           "write_line_record deve scrivere una riga normale");
    failures += expect_int(read_line_record(pipefd[0], &line_out) == 1,
                           "read_line_record deve leggere una riga normale");
    failures += expect_int(line_out.file_name_len == line_item.file_name_len,
                           "file_name_len deve essere preservata");
    failures += expect_int(strcmp(line_out.file_name, line_item.file_name) == 0,
                           "file_name deve essere preservato");
    failures += expect_int(line_out.line_number == line_item.line_number,
                           "line_number deve essere preservato");
    failures += expect_int(line_out.line_len == line_item.line_len,
                           "line_len deve essere preservata");
    failures += expect_int(strcmp(line_out.line, line_item.line) == 0,
                           "line deve essere preservata");
    free_line_item(&line_out);
    failures += expect_int(close_pair(pipefd) == 0, "close pipe round-trip riga deve riuscire");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe riga vuota deve riuscire");
    line_item.file_name = "empty.txt";
    line_item.file_name_len = strlen(line_item.file_name);
    line_item.line_number = 1;
    line_item.line = "";
    line_item.line_len = 0;
    failures += expect_int(write_line_record(pipefd[1], &line_item) == 0,
                           "write_line_record deve scrivere una riga vuota");
    failures += expect_int(read_line_record(pipefd[0], &line_out) == 1,
                           "read_line_record deve leggere una riga vuota");
    failures += expect_int(line_out.line_len == 0, "line_len della riga vuota deve essere zero");
    failures += expect_int(line_out.line != NULL && line_out.line[0] == '\0',
                           "riga vuota deve essere ricostruita come stringa vuota");
    free_line_item(&line_out);
    failures += expect_int(close_pair(pipefd) == 0, "close pipe riga vuota deve riuscire");

    failures += expect_int(line_queue_init(&queue, 2) == 0,
                           "line_queue_init deve riuscire per il test di ownership");
    if (queue.items != NULL) {
        queued_item.file_name = strdup("queued.txt");
        queued_item.line = strdup("linea in coda");
        queued_item.file_name_len = strlen("queued.txt");
        queued_item.line_len = strlen("linea in coda");
        queued_item.line_number = 5;

        failures += expect_int(queued_item.file_name != NULL && queued_item.line != NULL,
                               "allocazione item proprietario per la coda deve riuscire");

        if (queued_item.file_name != NULL && queued_item.line != NULL) {
            failures += expect_int(line_queue_push(&queue, &queued_item) == 0,
                                   "line_queue_push deve accettare un item proprietario");
            failures += expect_int(queued_item.file_name == NULL && queued_item.line == NULL,
                                   "line_queue_push riuscita deve azzerare l'item del chiamante");
            failures += expect_int(line_queue_pop(&queue, &popped_item) == 1,
                                   "line_queue_pop deve restituire l'item inserito");
            failures += expect_int(strcmp(popped_item.file_name, "queued.txt") == 0,
                                   "line_queue_pop deve trasferire il file_name al consumer");
            failures += expect_int(strcmp(popped_item.line, "linea in coda") == 0,
                                   "line_queue_pop deve trasferire la riga al consumer");
            free_line_item(&popped_item);
        }

        free_line_item(&queued_item);
        line_queue_destroy(&queue);
    }

    failures += expect_int(line_queue_init(&closed_queue, 1) == 0,
                           "line_queue_init deve riuscire per il test di push fallita");
    if (closed_queue.items != NULL) {
        rejected_item.file_name = strdup("rejected.txt");
        rejected_item.line = strdup("riga rifiutata");
        rejected_item.file_name_len = strlen("rejected.txt");
        rejected_item.line_len = strlen("riga rifiutata");
        rejected_item.line_number = 6;

        failures += expect_int(rejected_item.file_name != NULL && rejected_item.line != NULL,
                               "allocazione item rifiutato deve riuscire");

        if (rejected_item.file_name != NULL && rejected_item.line != NULL) {
            line_queue_close(&closed_queue);
            errno = 0;
            failures += expect_int(line_queue_push(&closed_queue, &rejected_item) == -1,
                                   "line_queue_push su coda chiusa deve fallire");
            failures += expect_int(errno == EPIPE,
                                   "line_queue_push su coda chiusa deve impostare EPIPE");
            failures += expect_int(rejected_item.file_name != NULL && rejected_item.line != NULL,
                                   "line_queue_push fallita deve lasciare ownership al chiamante");
        }

        free_line_item(&rejected_item);
        line_queue_destroy(&closed_queue);
    }

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe EOF read_line_record deve riuscire");
    failures += expect_int(close(pipefd[1]) == 0, "chiusura lato scrittura EOF deve riuscire");
    pipefd[1] = -1;
    failures += expect_int(read_line_record(pipefd[0], &line_out) == 0,
                           "read_line_record deve restituire 0 su EOF pulito");
    failures += expect_int(close_pair(pipefd) == 0, "close pipe EOF read_line_record deve riuscire");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe record riga troncato deve riuscire");
    mr_line_header_t bad_header = {7, 2, 9};
    failures += expect_int(writen(pipefd[1], &bad_header, sizeof(bad_header)) ==
                               (ssize_t)sizeof(bad_header),
                           "scrittura header del record riga troncato deve riuscire");
    failures += expect_int(writen(pipefd[1], "bad", 3) == 3,
                           "scrittura payload parziale del record riga troncato deve riuscire");
    failures += expect_int(close(pipefd[1]) == 0,
                           "chiusura lato scrittura record riga troncato deve riuscire");
    pipefd[1] = -1;
    errno = 0;
    failures += expect_int(read_line_record(pipefd[0], &line_out) == -1,
                           "read_line_record deve fallire su record riga troncato");
    failures += expect_int(errno == EPROTO,
                           "read_line_record su record riga troncato deve impostare EPROTO");
    failures += expect_int(close_pair(pipefd) == 0,
                           "close pipe record riga troncato deve riuscire");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe mapper_emit_pair deve riuscire");
    if (pipefd[0] != -1 && pipefd[1] != -1) {
        const unsigned char value[] = {'a', 0, 'b'};
        emit_lock_ready = mtx_init(&mapper_context.pipe_emit_lock, mtx_plain) == thrd_success;
        failures += expect_int(emit_lock_ready, "mutex emit mapper deve inizializzarsi");
        if (emit_lock_ready) {
            mapper_context.out_fd = pipefd[1];

            failures += expect_int(mapper_emit_pair("Alpha9", value, sizeof(value), &mapper_context) == 0,
                                   "mapper_emit_pair deve scrivere una coppia valida");
            failures += expect_pair_record(pipefd[0], "Alpha9", value, sizeof(value));

            failures += expect_int(mapper_emit_pair("Zero", NULL, 0, &mapper_context) == 0,
                                   "mapper_emit_pair deve accettare valore nullo di dimensione zero");
            failures += expect_pair_record(pipefd[0], "Zero", NULL, 0);

            errno = 0;
            failures += expect_int(mapper_emit_pair("bad-token", value, sizeof(value), &mapper_context) == -1,
                                   "mapper_emit_pair deve rifiutare token non alfanumerici");
            failures += expect_int(errno == EINVAL,
                                   "mapper_emit_pair con token invalido deve impostare EINVAL");

            errno = 0;
            failures += expect_int(mapper_emit_pair("NoValue", NULL, 1, &mapper_context) == -1,
                                   "mapper_emit_pair deve rifiutare value NULL con dimensione positiva");
            failures += expect_int(errno == EINVAL,
                                   "mapper_emit_pair con valore invalido deve impostare EINVAL");

            mtx_destroy(&mapper_context.pipe_emit_lock);
            emit_lock_ready = 0;
            mapper_context = (mr_mapper_context_t){0};
        }
        failures += expect_int(close_pair(pipefd) == 0,
                               "close pipe mapper_emit_pair deve riuscire");
    }

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe read_pair_record deve riuscire");
    if (pipefd[0] != -1 && pipefd[1] != -1) {
        const unsigned char value[] = {'r', 0, 'd'};
        emit_lock_ready = mtx_init(&mapper_context.pipe_emit_lock, mtx_plain) == thrd_success;
        failures += expect_int(emit_lock_ready,
                               "mutex emit mapper per read_pair_record deve inizializzarsi");
        if (emit_lock_ready) {
            mapper_context.out_fd = pipefd[1];

            failures += expect_int(mapper_emit_pair("ReadToken", value, sizeof(value),
                                                    &mapper_context) == 0,
                                   "mapper_emit_pair deve produrre input per read_pair_record");
            failures += expect_int(read_pair_record(pipefd[0], &pair_item) == 1,
                                   "read_pair_record deve leggere una coppia valida");
            failures += expect_pair_item(&pair_item, "ReadToken", value, sizeof(value));
            pair_item_destroy(&pair_item);

            failures += expect_int(mapper_emit_pair("EmptyValue", NULL, 0, &mapper_context) == 0,
                                   "mapper_emit_pair deve produrre un valore vuoto");
            failures += expect_int(read_pair_record(pipefd[0], &pair_item) == 1,
                                   "read_pair_record deve leggere un valore vuoto");
            failures += expect_pair_item(&pair_item, "EmptyValue", NULL, 0);
            pair_item_destroy(&pair_item);

            mtx_destroy(&mapper_context.pipe_emit_lock);
            emit_lock_ready = 0;
            mapper_context = (mr_mapper_context_t){0};
        }
        failures += expect_int(close_pair(pipefd) == 0,
                               "close pipe read_pair_record deve riuscire");
    }

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe EOF read_pair_record deve riuscire");
    failures += expect_int(close(pipefd[1]) == 0,
                           "chiusura lato scrittura EOF read_pair_record deve riuscire");
    pipefd[1] = -1;
    failures += expect_int(read_pair_record(pipefd[0], &pair_item) == 0,
                           "read_pair_record deve restituire 0 su EOF pulito");
    failures += expect_int(close_pair(pipefd) == 0,
                           "close pipe EOF read_pair_record deve riuscire");

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0,
                           "pipe header invalido read_pair_record deve riuscire");
    if (pipefd[0] != -1 && pipefd[1] != -1) {
        mr_pair_header_t invalid_header = {0, 1};

        failures += expect_int(writen(pipefd[1], &invalid_header, sizeof(invalid_header)) ==
                                   (ssize_t)sizeof(invalid_header),
                               "scrittura header coppia invalido deve riuscire");
        failures += expect_int(close(pipefd[1]) == 0,
                               "chiusura lato scrittura header invalido deve riuscire");
        pipefd[1] = -1;
        errno = 0;
        failures += expect_int(read_pair_record(pipefd[0], &pair_item) == -1,
                               "read_pair_record deve rifiutare header con token vuoto");
        failures += expect_int(errno == EPROTO,
                               "read_pair_record su header invalido deve impostare EPROTO");
        failures += expect_int(close_pair(pipefd) == 0,
                               "close pipe header invalido read_pair_record deve riuscire");
    }

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0,
                           "pipe record troncato read_pair_record deve riuscire");
    if (pipefd[0] != -1 && pipefd[1] != -1) {
        mr_pair_header_t truncated_header = {5, 3};

        failures += expect_int(writen(pipefd[1], &truncated_header, sizeof(truncated_header)) ==
                                   (ssize_t)sizeof(truncated_header),
                               "scrittura header coppia troncata deve riuscire");
        failures += expect_int(writen(pipefd[1], "abc", 3) == 3,
                               "scrittura payload parziale coppia deve riuscire");
        failures += expect_int(close(pipefd[1]) == 0,
                               "chiusura lato scrittura coppia troncata deve riuscire");
        pipefd[1] = -1;
        errno = 0;
        failures += expect_int(read_pair_record(pipefd[0], &pair_item) == -1,
                               "read_pair_record deve fallire su coppia troncata");
        failures += expect_int(errno == EPROTO,
                               "read_pair_record su coppia troncata deve impostare EPROTO");
        pair_item_destroy(&pair_item);
        failures += expect_int(close_pair(pipefd) == 0,
                               "close pipe record troncato read_pair_record deve riuscire");
    }

    mr_pair_groups_t groups = {0};
    {
        const unsigned char alpha_first[] = {'1', 0, 'a'};
        const unsigned char alpha_second[] = {'2', 0, 'b'};
        mr_pair_group_t *group = NULL;

        failures += expect_int(make_pair_item(&pair_item, "Alpha", alpha_first,
                                              sizeof(alpha_first)) == 0,
                               "creazione pair Alpha iniziale deve riuscire");
        if (pair_item.token != NULL && pair_item.value != NULL) {
            failures += expect_int(pair_groups_add_pair(&groups, &pair_item) == 0,
                                   "pair_groups_add_pair deve creare un gruppo per un token nuovo");
            failures += expect_int(pair_item.token == NULL && pair_item.value == NULL,
                                   "pair_groups_add_pair deve consumare la pair su token nuovo");
            failures += expect_int(groups.count == 1,
                                   "pair_groups_add_pair deve aggiungere un solo gruppo nuovo");
            group = pair_groups_find(&groups, "Alpha", strlen("Alpha"));
            failures += expect_int(group != NULL,
                                   "pair_groups_find deve trovare il gruppo appena creato");
            if (group != NULL) {
                failures += expect_int(group->values_count == 1,
                                       "gruppo nuovo deve contenere un valore");
                failures += expect_int(group->values[0].size == sizeof(alpha_first),
                                       "primo valore del gruppo deve preservare la dimensione");
                failures += expect_int(memcmp(group->values[0].data, alpha_first,
                                              sizeof(alpha_first)) == 0,
                                       "primo valore del gruppo deve preservare i byte opachi");
            }
        }
        pair_item_destroy(&pair_item);

        failures += expect_int(make_pair_item(&pair_item, "Alpha", alpha_second,
                                              sizeof(alpha_second)) == 0,
                               "creazione seconda pair Alpha deve riuscire");
        if (pair_item.token != NULL && pair_item.value != NULL) {
            failures += expect_int(pair_groups_add_pair(&groups, &pair_item) == 0,
                                   "pair_groups_add_pair deve aggiungere un valore a un gruppo esistente");
            failures += expect_int(pair_item.token == NULL && pair_item.value == NULL,
                                   "pair_groups_add_pair deve consumare la pair su token esistente");
            failures += expect_int(groups.count == 1,
                                   "token esistente non deve creare un secondo gruppo");
            group = pair_groups_find(&groups, "Alpha", strlen("Alpha"));
            failures += expect_int(group != NULL,
                                   "pair_groups_find deve ritrovare il gruppo esistente");
            if (group != NULL) {
                failures += expect_int(group->values_count == 2,
                                       "gruppo esistente deve contenere due valori");
                failures += expect_int(group->values[1].size == sizeof(alpha_second),
                                       "secondo valore del gruppo deve preservare la dimensione");
                failures += expect_int(memcmp(group->values[1].data, alpha_second,
                                              sizeof(alpha_second)) == 0,
                                       "secondo valore del gruppo deve preservare i byte opachi");
            }
        }
        pair_item_destroy(&pair_item);

        failures += expect_int(make_pair_item(&pair_item, "Beta", NULL, 0) == 0,
                               "creazione pair Beta con valore vuoto deve riuscire");
        if (pair_item.token != NULL) {
            failures += expect_int(pair_groups_add_pair(&groups, &pair_item) == 0,
                                   "pair_groups_add_pair deve accettare un valore vuoto");
            failures += expect_int(pair_item.token == NULL && pair_item.value == NULL,
                                   "pair_groups_add_pair deve consumare la pair con valore vuoto");
            failures += expect_int(groups.count == 2,
                                   "token diverso deve creare un secondo gruppo");
            group = pair_groups_find(&groups, "Beta", strlen("Beta"));
            failures += expect_int(group != NULL,
                                   "pair_groups_find deve trovare il gruppo con valore vuoto");
            if (group != NULL) {
                failures += expect_int(group->values_count == 1,
                                       "gruppo Beta deve contenere un valore");
                failures += expect_int(group->values[0].data == NULL,
                                       "valore vuoto deve mantenere data NULL");
                failures += expect_int(group->values[0].size == 0,
                                       "valore vuoto deve mantenere size zero");
            }
        }
        pair_item_destroy(&pair_item);
        pair_groups_destroy(&groups);
    }

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe collect_pair_groups deve riuscire");
    if (pipefd[0] != -1 && pipefd[1] != -1) {
        const unsigned char alpha_first[] = {'x', 0, '1'};
        const unsigned char alpha_second[] = {'y', 0, '2'};
        mr_pair_groups_t collected_groups = {0};
        mr_pair_group_t *group = NULL;

        emit_lock_ready = mtx_init(&mapper_context.pipe_emit_lock, mtx_plain) == thrd_success;
        failures += expect_int(emit_lock_ready,
                               "mutex emit mapper per collect_pair_groups deve inizializzarsi");
        if (emit_lock_ready) {
            mapper_context.out_fd = pipefd[1];

            failures += expect_int(mapper_emit_pair("Alpha", alpha_first, sizeof(alpha_first),
                                                    &mapper_context) == 0,
                                   "mapper_emit_pair deve produrre il primo valore Alpha");
            failures += expect_int(mapper_emit_pair("Beta", NULL, 0, &mapper_context) == 0,
                                   "mapper_emit_pair deve produrre un valore vuoto Beta");
            failures += expect_int(mapper_emit_pair("Alpha", alpha_second, sizeof(alpha_second),
                                                    &mapper_context) == 0,
                                   "mapper_emit_pair deve produrre il secondo valore Alpha");
            failures += expect_int(close(pipefd[1]) == 0,
                                   "chiusura lato scrittura collect_pair_groups deve riuscire");
            pipefd[1] = -1;

            failures += expect_int(collect_pair_groups(pipefd[0], &collected_groups) == 0,
                                   "collect_pair_groups deve leggere fino a EOF pulito");
            failures += expect_int(collected_groups.count == 2,
                                   "collect_pair_groups deve creare due gruppi distinti");

            group = pair_groups_find(&collected_groups, "Alpha", strlen("Alpha"));
            failures += expect_int(group != NULL,
                                   "collect_pair_groups deve raccogliere il gruppo Alpha");
            if (group != NULL) {
                failures += expect_int(group->values_count == 2,
                                       "gruppo Alpha raccolto deve contenere due valori");
                failures += expect_int(group->values[0].size == sizeof(alpha_first),
                                       "primo valore Alpha raccolto deve preservare la dimensione");
                failures += expect_int(memcmp(group->values[0].data, alpha_first,
                                              sizeof(alpha_first)) == 0,
                                       "primo valore Alpha raccolto deve preservare i byte");
                failures += expect_int(group->values[1].size == sizeof(alpha_second),
                                       "secondo valore Alpha raccolto deve preservare la dimensione");
                failures += expect_int(memcmp(group->values[1].data, alpha_second,
                                              sizeof(alpha_second)) == 0,
                                       "secondo valore Alpha raccolto deve preservare i byte");
            }

            group = pair_groups_find(&collected_groups, "Beta", strlen("Beta"));
            failures += expect_int(group != NULL,
                                   "collect_pair_groups deve raccogliere il gruppo Beta");
            if (group != NULL) {
                failures += expect_int(group->values_count == 1,
                                       "gruppo Beta raccolto deve contenere un valore");
                failures += expect_int(group->values[0].data == NULL,
                                       "valore vuoto Beta raccolto deve mantenere data NULL");
                failures += expect_int(group->values[0].size == 0,
                                       "valore vuoto Beta raccolto deve mantenere size zero");
            }

            pair_groups_destroy(&collected_groups);
            mtx_destroy(&mapper_context.pipe_emit_lock);
            emit_lock_ready = 0;
            mapper_context = (mr_mapper_context_t){0};
        }
        failures += expect_int(close_pair(pipefd) == 0,
                               "close pipe collect_pair_groups deve riuscire");
    }

    {
        int reducer_input[2] = {-1, -1};
        int reducer_output[2] = {-1, -1};
        int saved_stdin = -1;
        int saved_stdout = -1;
        mr_attr_t reducer_attr = {0};
        mr_t reducer_mr = NULL;
        const int alpha_first = 1;
        const int alpha_second = 2;
        const int beta_value = 5;
        const int gamma_value = 7;
        const int alpha_total = 3;
        const int beta_total = 5;
        const int gamma_total = 7;
        unsigned char extra_result_byte = 0;
        int reducer_status = -1;

        failures += expect_int(pipe(reducer_input) == 0,
                               "pipe input reducer_process_main deve riuscire");
        failures += expect_int(pipe(reducer_output) == 0,
                               "pipe output reducer_process_main deve riuscire");

        if (reducer_input[0] != -1 && reducer_input[1] != -1 &&
            reducer_output[0] != -1 && reducer_output[1] != -1) {
            emit_lock_ready = mtx_init(&mapper_context.pipe_emit_lock, mtx_plain) == thrd_success;
            failures += expect_int(emit_lock_ready,
                                   "mutex emit mapper per reducer_process_main deve inizializzarsi");
            if (emit_lock_ready) {
                mapper_context.out_fd = reducer_input[1];

                failures += expect_int(mapper_emit_pair("Gamma", &gamma_value, sizeof(gamma_value),
                                                        &mapper_context) == 0,
                                       "reducer_process_main deve ricevere il valore Gamma");
                failures += expect_int(mapper_emit_pair("Alpha", &alpha_first, sizeof(alpha_first),
                                                        &mapper_context) == 0,
                                       "reducer_process_main deve ricevere il primo valore Alpha");
                failures += expect_int(mapper_emit_pair("Beta", &beta_value, sizeof(beta_value),
                                                        &mapper_context) == 0,
                                       "reducer_process_main deve ricevere il valore Beta");
                failures += expect_int(mapper_emit_pair("Alpha", &alpha_second, sizeof(alpha_second),
                                                        &mapper_context) == 0,
                                       "reducer_process_main deve ricevere il secondo valore Alpha");

                mtx_destroy(&mapper_context.pipe_emit_lock);
                emit_lock_ready = 0;
                mapper_context = (mr_mapper_context_t){0};
            }

            failures += expect_int(close(reducer_input[1]) == 0,
                                   "chiusura input reducer_process_main deve inviare EOF");
            reducer_input[1] = -1;

            failures += expect_int(mr_attr_init(&reducer_attr) == 0,
                                   "mr_attr_init per reducer_process_main deve riuscire");
            failures += expect_int(mr_attr_set_reducer_threads(&reducer_attr, 2) == 0,
                                   "reducer_process_main deve usare due worker reducer");
            failures += expect_int(mr_create(&reducer_mr, &reducer_attr, test_mapper, sum_reducer,
                                             NULL) == 0,
                                   "mr_create per reducer_process_main deve riuscire");

            saved_stdin = dup(STDIN_FILENO);
            saved_stdout = dup(STDOUT_FILENO);
            failures += expect_int(saved_stdin != -1,
                                   "dup stdin per reducer_process_main deve riuscire");
            failures += expect_int(saved_stdout != -1,
                                   "dup stdout per reducer_process_main deve riuscire");

            if (reducer_mr != NULL && saved_stdin != -1 && saved_stdout != -1) {
                int redirected = 1;

                if (dup2(reducer_input[0], STDIN_FILENO) == -1) {
                    failures += expect_int(0, "dup2 input reducer_process_main deve riuscire");
                    redirected = 0;
                }
                if (dup2(reducer_output[1], STDOUT_FILENO) == -1) {
                    failures += expect_int(0, "dup2 output reducer_process_main deve riuscire");
                    redirected = 0;
                }

                if (redirected) {
                    failures += expect_int(close(reducer_input[0]) == 0,
                                           "chiusura fd input originale reducer_process_main deve riuscire");
                    reducer_input[0] = -1;
                    failures += expect_int(close(reducer_output[1]) == 0,
                                           "chiusura fd output originale reducer_process_main deve riuscire");
                    reducer_output[1] = -1;

                    reducer_status = reducer_process_main(reducer_mr, -1);
                }

                failures += expect_int(dup2(saved_stdin, STDIN_FILENO) != -1,
                                       "ripristino stdin dopo reducer_process_main deve riuscire");
                failures += expect_int(dup2(saved_stdout, STDOUT_FILENO) != -1,
                                       "ripristino stdout dopo reducer_process_main deve riuscire");

                if (redirected) {
                    failures += expect_int(reducer_status == 0,
                                           "reducer_process_main deve completare la riduzione");
                    failures += expect_result_record(reducer_output[0], "Alpha", &alpha_total,
                                                     sizeof(alpha_total));
                    failures += expect_result_record(reducer_output[0], "Beta", &beta_total,
                                                     sizeof(beta_total));
                    failures += expect_result_record(reducer_output[0], "Gamma", &gamma_total,
                                                     sizeof(gamma_total));
                    failures += expect_int(readn(reducer_output[0], &extra_result_byte, 1) == 0,
                                           "reducer_process_main multithread non deve produrre record extra");
                }
            }
        }

        if (saved_stdin != -1) { failures += expect_int(close(saved_stdin) == 0,
                                                        "close saved stdin deve riuscire"); }
        if (saved_stdout != -1) { failures += expect_int(close(saved_stdout) == 0,
                                                         "close saved stdout deve riuscire"); }
        if (reducer_mr != NULL) {
            failures += expect_int(mr_destroy(reducer_mr) == 0,
                                   "mr_destroy per reducer_process_main deve riuscire");
        }
        failures += expect_int(close_pair(reducer_input) == 0,
                               "close input reducer_process_main deve riuscire");
        failures += expect_int(close_pair(reducer_output) == 0,
                               "close output reducer_process_main deve riuscire");
    }

    pipefd[0] = -1;
    pipefd[1] = -1;
    failures += expect_int(pipe(pipefd) == 0, "pipe mapper_worker_main deve riuscire");
    if (pipefd[0] != -1 && pipefd[1] != -1) {
        queued_item.file_name = strdup("worker.txt");
        queued_item.line = strdup("zeta");
        queued_item.file_name_len = strlen("worker.txt");
        queued_item.line_len = strlen("zeta");
        queued_item.line_number = 3;

        failures += expect_int(queued_item.file_name != NULL && queued_item.line != NULL,
                               "allocazione item per mapper_worker_main deve riuscire");
        worker_queue_ready = line_queue_init(&mapper_context.queue, 1) == 0;
        failures += expect_int(worker_queue_ready, "coda mapper_worker_main deve inizializzarsi");
        worker_lock_ready = mtx_init(&mapper_context.pipe_emit_lock, mtx_plain) == thrd_success;
        failures += expect_int(worker_lock_ready, "mutex mapper_worker_main deve inizializzarsi");

        if (queued_item.file_name != NULL && queued_item.line != NULL &&
            worker_queue_ready && worker_lock_ready) {
            const unsigned char expected_value[] = {'z', 0, 4};

            mapper_context.mapper = test_mapper;
            mapper_context.user_arg = NULL;
            mapper_context.out_fd = pipefd[1];
            mapper_context.log_fd = -1;
            mr_mapper_worker_arg_t worker_arg = {
                .context = &mapper_context,
                .index = 0,
            };

            failures += expect_int(line_queue_push(&mapper_context.queue, &queued_item) == 0,
                                   "push item per mapper_worker_main deve riuscire");
            line_queue_close(&mapper_context.queue);
            failures += expect_int(mapper_worker_main(&worker_arg) == 0,
                                   "mapper_worker_main deve consumare la coda chiusa");
            failures += expect_pair_record(pipefd[0], "WorkerToken", expected_value,
                                           sizeof(expected_value));
        }

        free_line_item(&queued_item);
        if (worker_queue_ready) {
            line_queue_destroy(&mapper_context.queue);
            worker_queue_ready = 0;
        }
        if (worker_lock_ready) {
            mtx_destroy(&mapper_context.pipe_emit_lock);
            worker_lock_ready = 0;
        }
        mapper_context = (mr_mapper_context_t){0};
        failures += expect_int(close_pair(pipefd) == 0,
                               "close pipe mapper_worker_main deve riuscire");
    }

    input_fd = mkstemp(input_path);
    failures += expect_int(input_fd != -1, "mkstemp per write_file_lines deve riuscire");
    if (input_fd != -1) {
        failures += expect_int(writen(input_fd, input_content, strlen(input_content)) ==
                                   (ssize_t)strlen(input_content),
                               "scrittura file temporaneo per write_file_lines deve riuscire");
        failures += expect_int(close(input_fd) == 0,
                               "chiusura file temporaneo per write_file_lines deve riuscire");
        input_fd = -1;

        pipefd[0] = -1;
        pipefd[1] = -1;
        failures += expect_int(pipe(pipefd) == 0, "pipe write_file_lines deve riuscire");
        if (pipefd[0] != -1 && pipefd[1] != -1) {
            failures += expect_int(write_file_lines(pipefd[1], input_path, "input.txt", -1, NULL, NULL) == 0,
                                   "write_file_lines deve scrivere tutte le righe del file");
            failures += expect_int(close(pipefd[1]) == 0,
                                   "chiusura lato scrittura write_file_lines deve riuscire");
            pipefd[1] = -1;

            failures += expect_line_record(pipefd[0], "input.txt", 1, "alpha");
            failures += expect_line_record(pipefd[0], "input.txt", 2, "");
            failures += expect_line_record(pipefd[0], "input.txt", 3, "beta");
            failures += expect_line_record(pipefd[0], "input.txt", 4, "gamma");
            failures += expect_int(read_line_record(pipefd[0], &line_out) == 0,
                                   "write_file_lines deve terminare con EOF pulito");
            failures += expect_int(close_pair(pipefd) == 0,
                                   "close pipe write_file_lines deve riuscire");
        }

        failures += expect_int(unlink(input_path) == 0,
                               "rimozione file temporaneo write_file_lines deve riuscire");
    }

    single_input_fd = mkstemp(single_input_path);
    failures += expect_int(single_input_fd != -1,
                           "mkstemp per write_input_path_lines su file singolo deve riuscire");
    if (single_input_fd != -1) {
        const char single_content[] = "solo\nfile";
        const char *single_file_name = strrchr(single_input_path, '/');
        single_file_name = single_file_name == NULL ? single_input_path : single_file_name + 1;

        failures += expect_int(writen(single_input_fd, single_content, strlen(single_content)) ==
                                   (ssize_t)strlen(single_content),
                               "scrittura file temporaneo singolo deve riuscire");
        failures += expect_int(close(single_input_fd) == 0,
                               "chiusura file temporaneo singolo deve riuscire");
        single_input_fd = -1;

        pipefd[0] = -1;
        pipefd[1] = -1;
        failures += expect_int(pipe(pipefd) == 0,
                               "pipe write_input_path_lines file singolo deve riuscire");
        if (pipefd[0] != -1 && pipefd[1] != -1) {
            failures += expect_int(write_input_path_lines(pipefd[1], single_input_path, -1, NULL, NULL) == 0,
                                   "write_input_path_lines deve accettare un file regolare");
            failures += expect_int(close(pipefd[1]) == 0,
                                   "chiusura lato scrittura file singolo deve riuscire");
            pipefd[1] = -1;

            failures += expect_line_record(pipefd[0], single_file_name, 1, "solo");
            failures += expect_line_record(pipefd[0], single_file_name, 2, "file");
            failures += expect_int(read_line_record(pipefd[0], &line_out) == 0,
                                   "file singolo deve terminare con EOF pulito");
            failures += expect_int(close_pair(pipefd) == 0,
                                   "close pipe file singolo deve riuscire");
        }

        failures += expect_int(unlink(single_input_path) == 0,
                               "rimozione file temporaneo singolo deve riuscire");
    }

    char *created_directory = mkdtemp(directory_path);
    failures += expect_int(created_directory != NULL,
                           "mkdtemp per write_input_path_lines directory deve riuscire");
    if (created_directory != NULL) {
        char a_path[256];
        char b_path[256];
        char ignored_dir_path[256];
        char ignored_file_path[256];
        char fifo_path[256];

        failures += expect_int(join_path(a_path, sizeof(a_path), created_directory, "a.txt") == 0,
                               "costruzione path a.txt deve riuscire");
        failures += expect_int(join_path(b_path, sizeof(b_path), created_directory, "b.txt") == 0,
                               "costruzione path b.txt deve riuscire");
        failures += expect_int(join_path(ignored_dir_path, sizeof(ignored_dir_path), created_directory,
                                         "subdir") == 0,
                               "costruzione path subdir deve riuscire");
        failures += expect_int(join_path(ignored_file_path, sizeof(ignored_file_path), ignored_dir_path,
                                         "ignored.txt") == 0,
                               "costruzione path ignored.txt deve riuscire");
        failures += expect_int(join_path(fifo_path, sizeof(fifo_path), created_directory, "fifo") == 0,
                               "costruzione path fifo deve riuscire");

        failures += expect_int(create_text_file(b_path, "bravo\n") == 0,
                               "creazione b.txt deve riuscire");
        failures += expect_int(create_text_file(a_path, "alpha\n") == 0,
                               "creazione a.txt deve riuscire");
        failures += expect_int(mkdir(ignored_dir_path, 0700) == 0,
                               "creazione sottodirectory ignorata deve riuscire");
        failures += expect_int(create_text_file(ignored_file_path, "ignored\n") == 0,
                               "creazione file in sottodirectory deve riuscire");
        failures += expect_int(mkfifo(fifo_path, 0600) == 0,
                               "creazione FIFO non supportata deve riuscire");

        pipefd[0] = -1;
        pipefd[1] = -1;
        failures += expect_int(pipe(pipefd) == 0,
                               "pipe write_input_path_lines directory deve riuscire");
        if (pipefd[0] != -1 && pipefd[1] != -1) {
            failures += expect_int(write_input_path_lines(pipefd[1], created_directory, -1, NULL, NULL) == 0,
                                   "write_input_path_lines deve accettare una directory");
            failures += expect_int(close(pipefd[1]) == 0,
                                   "chiusura lato scrittura directory deve riuscire");
            pipefd[1] = -1;

            failures += expect_line_record(pipefd[0], "a.txt", 1, "alpha");
            failures += expect_line_record(pipefd[0], "b.txt", 1, "bravo");
            failures += expect_int(read_line_record(pipefd[0], &line_out) == 0,
                                   "directory deve terminare con EOF pulito");
            failures += expect_int(close_pair(pipefd) == 0,
                                   "close pipe directory deve riuscire");
        }

        pipefd[0] = -1;
        pipefd[1] = -1;
        failures += expect_int(pipe(pipefd) == 0,
                               "pipe write_input_path_lines tipo non supportato deve riuscire");
        if (pipefd[0] != -1 && pipefd[1] != -1) {
            errno = 0;
            failures += expect_int(write_input_path_lines(pipefd[1], fifo_path, -1, NULL, NULL) == -1,
                                   "write_input_path_lines deve rifiutare un path non supportato");
            failures += expect_int(errno == EINVAL,
                                   "write_input_path_lines su tipo non supportato deve impostare EINVAL");
            failures += expect_int(close_pair(pipefd) == 0,
                                   "close pipe tipo non supportato deve riuscire");
        }

        failures += expect_int(unlink(fifo_path) == 0,
                               "rimozione FIFO deve riuscire");
        failures += expect_int(unlink(ignored_file_path) == 0,
                               "rimozione file ignorato deve riuscire");
        failures += expect_int(rmdir(ignored_dir_path) == 0,
                               "rimozione sottodirectory deve riuscire");
        failures += expect_int(unlink(a_path) == 0,
                               "rimozione a.txt deve riuscire");
        failures += expect_int(unlink(b_path) == 0,
                               "rimozione b.txt deve riuscire");
        failures += expect_int(rmdir(created_directory) == 0,
                               "rimozione directory temporanea deve riuscire");
    }

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_io: ok\n");
    return 0;
}
