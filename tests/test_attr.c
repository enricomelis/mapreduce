#include "mr.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int expect_int(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }

    return 0;
}

int main(void) {
    mr_attr_t attr;
    int failures = 0;
    const char *log_path = "custom.log";

    errno = 0;
    failures += expect_int(mr_attr_init(NULL) == -1, "mr_attr_init(NULL) deve fallire");
    failures += expect_int(errno == EINVAL, "mr_attr_init(NULL) deve impostare errno a EINVAL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(attr.mapper_threads == 1, "mapper_threads default deve essere 1");
    failures += expect_int(attr.reducer_threads == 1, "reducer_threads default deve essere 1");
    failures += expect_int(attr.queue_size == 64, "queue_size default deve essere 64");
    failures += expect_int(strcmp(attr.log_file, "mr.log") == 0,
                           "log_file default deve essere mr.log");

    failures += expect_int(mr_attr_set_mapper_threads(&attr, 4) == 0,
                           "mr_attr_set_mapper_threads deve accettare valori positivi");
    failures += expect_int(attr.mapper_threads == 4,
                           "mapper_threads deve essere aggiornato dal setter");

    failures += expect_int(mr_attr_set_reducer_threads(&attr, 3) == 0,
                           "mr_attr_set_reducer_threads deve accettare valori positivi");
    failures += expect_int(attr.reducer_threads == 3,
                           "reducer_threads deve essere aggiornato dal setter");

    failures += expect_int(mr_attr_set_queue_size(&attr, 128) == 0,
                           "mr_attr_set_queue_size deve accettare valori positivi");
    failures += expect_int(attr.queue_size == 128,
                           "queue_size deve essere aggiornato dal setter");

    failures += expect_int(mr_attr_set_log_file(&attr, log_path) == 0,
                           "mr_attr_set_log_file deve accettare un path");
    failures += expect_int(attr.log_file == log_path,
                           "log_file deve essere aggiornato dal setter");

    failures += expect_int(mr_attr_set_log_file(&attr, NULL) == 0,
                           "mr_attr_set_log_file deve accettare NULL per il default");
    failures += expect_int(strcmp(attr.log_file, "mr.log") == 0,
                           "log_file deve tornare al default");

    errno = 0;
    failures += expect_int(mr_attr_set_mapper_threads(NULL, 1) == -1,
                           "mr_attr_set_mapper_threads(NULL, 1) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_mapper_threads(NULL, 1) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_mapper_threads(&attr, 0) == -1,
                           "mr_attr_set_mapper_threads(&attr, 0) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_mapper_threads(&attr, 0) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_reducer_threads(NULL, 1) == -1,
                           "mr_attr_set_reducer_threads(NULL, 1) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_reducer_threads(NULL, 1) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_reducer_threads(&attr, 0) == -1,
                           "mr_attr_set_reducer_threads(&attr, 0) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_reducer_threads(&attr, 0) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_queue_size(NULL, 1) == -1,
                           "mr_attr_set_queue_size(NULL, 1) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_queue_size(NULL, 1) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_queue_size(&attr, 0) == -1,
                           "mr_attr_set_queue_size(&attr, 0) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_queue_size(&attr, 0) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_log_file(NULL, "x.log") == -1,
                           "mr_attr_set_log_file(NULL, path) deve fallire");
    failures += expect_int(errno == EINVAL,
                           "mr_attr_set_log_file(NULL, path) deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_destroy(NULL) == -1, "mr_attr_destroy(NULL) deve fallire");
    failures += expect_int(errno == EINVAL, "mr_attr_destroy(NULL) deve impostare errno a EINVAL");

    failures += expect_int(mr_attr_destroy(&attr) == 0, "mr_attr_destroy deve riuscire");
    failures += expect_int(attr.mapper_threads == 0, "mapper_threads dopo destroy deve essere 0");
    failures += expect_int(attr.reducer_threads == 0, "reducer_threads dopo destroy deve essere 0");
    failures += expect_int(attr.queue_size == 0, "queue_size dopo destroy deve essere 0");
    failures += expect_int(attr.log_file == NULL, "log_file dopo destroy deve essere NULL");

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_attr: ok\n");
    return 0;
}
