#include "../src/mr_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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
    int user_value = 7;
    int failures = 0;

    errno = 0;
    failures += expect_int(mr_attr_init(NULL) == -1, "mr_attr_init(NULL) deve fallire");
    failures += expect_int(errno == EINVAL, "mr_attr_init(NULL) deve impostare EINVAL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    failures += expect_int(attr.mapper_threads == 1, "mapper_threads default deve essere 1");
    failures += expect_int(attr.reducer_threads == 1, "reducer_threads default deve essere 1");
    failures += expect_int(attr.queue_size == 64, "queue_size default deve essere 64");
    failures += expect_int(strcmp(attr.log_file, "mr.log") == 0, "log_file default deve essere mr.log");

    failures += expect_int(mr_attr_set_mapper_threads(&attr, 4) == 0,
                           "mapper_threads deve accettare valori positivi");
    failures += expect_int(attr.mapper_threads == 4, "mapper_threads deve essere aggiornato");

    failures += expect_int(mr_attr_set_reducer_threads(&attr, 3) == 0,
                           "reducer_threads deve accettare valori positivi");
    failures += expect_int(attr.reducer_threads == 3, "reducer_threads deve essere aggiornato");

    failures += expect_int(mr_attr_set_queue_size(&attr, 8) == 0,
                           "queue_size deve accettare valori positivi");
    failures += expect_int(attr.queue_size == 8, "queue_size deve essere aggiornato");

    failures += expect_int(mr_attr_set_log_file(&attr, "custom.log") == 0,
                           "log_file deve accettare un path valido");
    failures += expect_int(strcmp(attr.log_file, "custom.log") == 0,
                           "log_file deve essere aggiornato");

    failures += expect_int(mr_attr_set_log_file(&attr, NULL) == 0,
                           "log_file deve accettare NULL come default");
    failures += expect_int(strcmp(attr.log_file, "mr.log") == 0,
                           "log_file NULL deve ripristinare mr.log");

    errno = 0;
    failures += expect_int(mr_attr_set_mapper_threads(NULL, 1) == -1,
                           "setter mapper deve rifiutare attr NULL");
    failures += expect_int(errno == EINVAL, "setter mapper NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_mapper_threads(&attr, 0) == -1,
                           "setter mapper deve rifiutare zero");
    failures += expect_int(errno == EINVAL, "setter mapper zero deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_reducer_threads(NULL, 1) == -1,
                           "setter reducer deve rifiutare attr NULL");
    failures += expect_int(errno == EINVAL, "setter reducer NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_reducer_threads(&attr, 0) == -1,
                           "setter reducer deve rifiutare zero");
    failures += expect_int(errno == EINVAL, "setter reducer zero deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_queue_size(NULL, 1) == -1,
                           "setter queue deve rifiutare attr NULL");
    failures += expect_int(errno == EINVAL, "setter queue NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_queue_size(&attr, 0) == -1,
                           "setter queue deve rifiutare zero");
    failures += expect_int(errno == EINVAL, "setter queue zero deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_attr_set_log_file(NULL, "x.log") == -1,
                           "setter log deve rifiutare attr NULL");
    failures += expect_int(errno == EINVAL, "setter log NULL deve impostare EINVAL");

    failures += expect_int(mr_attr_init(&attr) == 0, "mr_attr_init deve ripristinare attr valida");

    errno = 0;
    failures += expect_int(mr_create(NULL, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare handle NULL");
    failures += expect_int(errno == EINVAL, "mr_create handle NULL deve impostare EINVAL");

    errno = 0;
    failures += expect_int(mr_create(&mr, NULL, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare attr NULL");
    failures += expect_int(errno == EINVAL, "mr_create attr NULL deve impostare EINVAL");
    failures += expect_int(mr == NULL, "mr_create fallita deve lasciare handle NULL");

    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, NULL, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare mapper NULL");
    failures += expect_int(errno == EINVAL, "mr_create mapper NULL deve impostare EINVAL");
    failures += expect_int(mr == NULL, "mr_create mapper NULL deve lasciare handle NULL");

    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, NULL, &user_value) == -1,
                           "mr_create deve rifiutare reducer NULL");
    failures += expect_int(errno == EINVAL, "mr_create reducer NULL deve impostare EINVAL");
    failures += expect_int(mr == NULL, "mr_create reducer NULL deve lasciare handle NULL");

    attr.mapper_threads = 0;
    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare mapper_threads zero");
    failures += expect_int(errno == EINVAL, "mapper_threads zero deve impostare EINVAL");
    failures += expect_int(mr == NULL, "mapper_threads zero deve lasciare handle NULL");

    failures += expect_int(mr_attr_init(&attr) == 0, "attr deve essere reinizializzata");
    attr.reducer_threads = 0;
    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare reducer_threads zero");
    failures += expect_int(errno == EINVAL, "reducer_threads zero deve impostare EINVAL");
    failures += expect_int(mr == NULL, "reducer_threads zero deve lasciare handle NULL");

    failures += expect_int(mr_attr_init(&attr) == 0, "attr deve essere reinizializzata ancora");
    attr.queue_size = 0;
    errno = 0;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == -1,
                           "mr_create deve rifiutare queue_size zero");
    failures += expect_int(errno == EINVAL, "queue_size zero deve impostare EINVAL");
    failures += expect_int(mr == NULL, "queue_size zero deve lasciare handle NULL");

    failures += expect_int(mr_attr_init(&attr) == 0, "attr finale deve essere valida");
    attr.log_file = NULL;
    failures += expect_int(mr_create(&mr, &attr, dummy_mapper, dummy_reducer, &user_value) == 0,
                           "mr_create deve riuscire con parametri validi");
    failures += expect_int(mr != NULL, "mr_create valida deve produrre un handle");
    if (mr != NULL) {
        failures += expect_int(mr->attr.mapper_threads == 1, "mr_create deve copiare mapper_threads");
        failures += expect_int(mr->attr.reducer_threads == 1, "mr_create deve copiare reducer_threads");
        failures += expect_int(mr->attr.queue_size == 64, "mr_create deve copiare queue_size");
        failures += expect_int(strcmp(mr->attr.log_file, "mr.log") == 0,
                               "mr_create deve applicare mr.log se attr.log_file e NULL");
        failures += expect_int(mr->mapper == dummy_mapper, "mr_create deve salvare mapper");
        failures += expect_int(mr->reducer == dummy_reducer, "mr_create deve salvare reducer");
        failures += expect_int(mr->user_arg == &user_value, "mr_create deve salvare user_arg");
        failures += expect_int(mr_destroy(mr) == 0, "mr_destroy deve riuscire");
        mr = NULL;
    }

    errno = 0;
    failures += expect_int(mr_destroy(NULL) == -1, "mr_destroy deve rifiutare NULL");
    failures += expect_int(errno == EINVAL, "mr_destroy NULL deve impostare EINVAL");

    failures += expect_int(mr_attr_destroy(&attr) == 0, "mr_attr_destroy deve riuscire");
    failures += expect_int(attr.mapper_threads == 0, "destroy deve azzerare mapper_threads");
    failures += expect_int(attr.reducer_threads == 0, "destroy deve azzerare reducer_threads");
    failures += expect_int(attr.queue_size == 0, "destroy deve azzerare queue_size");
    failures += expect_int(attr.log_file == NULL, "destroy deve azzerare log_file");

    errno = 0;
    failures += expect_int(mr_attr_destroy(NULL) == -1, "mr_attr_destroy NULL deve fallire");
    failures += expect_int(errno == EINVAL, "mr_attr_destroy NULL deve impostare EINVAL");

    if (failures != 0) {
        fprintf(stderr, "%d controlli falliti\n", failures);
        return 1;
    }

    printf("test_attr_lifecycle: ok\n");
    return 0;
}
