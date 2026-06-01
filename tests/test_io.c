#include <errno.h>
#include <stdio.h>
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

int main(void) {
    int pipefd[2] = {-1, -1};
    const char message[] = "record-binario";
    char buffer[sizeof(message)] = {0};
    char partial[8] = {0};
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

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_io: ok\n");
    return 0;
}
