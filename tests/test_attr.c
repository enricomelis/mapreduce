#include "mr.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>

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

    errno = 0;
    failures += expect_int(mr_attr_init(NULL) == -1, "mr_attr_init(NULL) deve fallire");
    failures += expect_int(errno == EINVAL, "mr_attr_init(NULL) deve impostare errno a EINVAL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(attr.mapper_threads == 1, "mapper_threads default deve essere 1");
    failures += expect_int(attr.reducer_threads == 1, "reducer_threads default deve essere 1");
    failures += expect_int(attr.queue_size == 64, "queue_size default deve essere 64");
    failures += expect_int(attr.log_file == NULL, "log_file default deve essere NULL");

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
