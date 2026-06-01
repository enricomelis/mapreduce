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

static int dummy_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                        void *user_arg) {
    (void)line;
    (void)emit;
    (void)emit_arg;
    (void)user_arg;
    return 0;
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
    int user_value = 42;
    int failures = 0;

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");

    errno = 0;
    failures += expect_int(mr_create(NULL, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare mr NULL");
    failures += expect_int(errno == EINVAL, "mr_create con mr NULL deve impostare errno a EINVAL");

    errno = 0;
    failures += expect_int(mr_create(&mr, NULL, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare attr NULL");
    failures += expect_int(errno == EINVAL,
                           "mr_create con attr NULL deve impostare errno a EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare mr a NULL");

    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, NULL, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare mapper NULL");
    failures += expect_int(errno == EINVAL,
                           "mr_create con mapper NULL deve impostare errno a EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare mr a NULL");

    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, NULL, &user_value) == -1,
                           "mr_create deve rifiutare reducer NULL");
    failures += expect_int(errno == EINVAL,
                           "mr_create con reducer NULL deve impostare errno a EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare mr a NULL");

    attr.mapper_threads = 0;
    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare mapper_threads zero");
    failures += expect_int(errno == EINVAL,
                           "mr_create con mapper_threads zero deve impostare errno a EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare mr a NULL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve ripristinare attr");
    attr.reducer_threads = 0;
    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare reducer_threads zero");
    failures += expect_int(errno == EINVAL,
                           "mr_create con reducer_threads zero deve impostare errno a EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare mr a NULL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve ripristinare attr");
    attr.queue_size = 0;
    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare queue_size zero");
    failures += expect_int(errno == EINVAL,
                           "mr_create con queue_size zero deve impostare errno a EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare mr a NULL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve ripristinare attr");
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == 0,
                           "mr_create deve riuscire con parametri validi");
    failures += expect_int(mr != NULL, "mr_create valida deve produrre un handle non NULL");

    if (mr != NULL) {
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy deve riuscire");
        mr = NULL;
    }

    errno = 0;
    failures += expect_int(mr_destroy(NULL) == -1, "mr_destroy deve rifiutare NULL");
    failures += expect_int(errno == EINVAL, "mr_destroy NULL deve impostare errno a EINVAL");

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_lifecycle: ok\n");
    return 0;
}
