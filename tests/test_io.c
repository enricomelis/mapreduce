#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/mr.c"

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

    free(item->file_name);
    free(item->line);
    *item = (mr_line_item_t){0};
}

int main(void) {
    int pipefd[2] = {-1, -1};
    const char message[] = "record-binario";
    char buffer[sizeof(message)] = {0};
    char partial[8] = {0};
    mr_line_item_t line_item = {0};
    mr_line_item_t line_out = {0};
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

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_io: ok\n");
    return 0;
}
