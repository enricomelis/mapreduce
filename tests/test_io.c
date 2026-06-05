#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    int input_fd = -1;
    int single_input_fd = -1;
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
            failures += expect_int(write_file_lines(pipefd[1], input_path, "input.txt") == 0,
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
            failures += expect_int(write_input_path_lines(pipefd[1], single_input_path) == 0,
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
            failures += expect_int(write_input_path_lines(pipefd[1], created_directory) == 0,
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
            failures += expect_int(write_input_path_lines(pipefd[1], fifo_path) == -1,
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
