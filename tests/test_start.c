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
    int failures = 0;

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, NULL) == 0,
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
    failures += expect_int(mr_start(mr, "input.txt", "output.mro") == -1,
                           "mr_start valida deve fallire finche non e implementata");
    failures += expect_int(errno == ENOSYS, "mr_start non implementata deve impostare ENOSYS");

    if (mr != NULL) {
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy deve riuscire");
    }

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_start: ok\n");
    return 0;
}
