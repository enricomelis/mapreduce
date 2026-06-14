#include "mr_internal.h"

#include <errno.h>
#include <stdlib.h>

int mr_attr_init(mr_attr_t *attr) {
    MR_CHECK_NULL(attr);

    attr->mapper_threads = 1;
    attr->reducer_threads = 1;
    attr->queue_size = 64;
    attr->log_file = MR_DEFAULT_LOG_FILE;

    return 0;
}

int mr_attr_destroy(mr_attr_t *attr) {
    MR_CHECK_NULL(attr);

    attr->mapper_threads = 0;
    attr->reducer_threads = 0;
    attr->queue_size = 0;
    attr->log_file = NULL;

    return 0;
}

int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n) {
    MR_CHECK_NULL(attr);
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    attr->mapper_threads = n;
    return 0;
}

int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n) {
    MR_CHECK_NULL(attr);
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    attr->reducer_threads = n;
    return 0;
}

int mr_attr_set_queue_size(mr_attr_t *attr, size_t n) {
    MR_CHECK_NULL(attr);
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    attr->queue_size = n;
    return 0;
}

int mr_attr_set_log_file(mr_attr_t *attr, const char *path) {
    MR_CHECK_NULL(attr);

    attr->log_file = path != NULL ? path : MR_DEFAULT_LOG_FILE;
    return 0;
}

int mr_create(mr_t *mr, const mr_attr_t *attr, mr_mapper_t mapper, mr_reducer_t reducer, void *user_arg) {
    MR_CHECK_NULL(mr);
    MR_CHECK_NULL(attr);
    MR_CHECK_NULL(mapper);
    MR_CHECK_NULL(reducer);

    *mr = NULL;

    if (attr->mapper_threads == 0 || attr->reducer_threads == 0 || attr->queue_size == 0) {
        errno = EINVAL;
        return -1;
    }

    mr_t mapreduce = malloc(sizeof(*mapreduce));
    if (mapreduce == NULL) { return -1; }

    mapreduce->attr = *attr;
    if (mapreduce->attr.log_file == NULL) { mapreduce->attr.log_file = MR_DEFAULT_LOG_FILE; }
    mapreduce->mapper = mapper;
    mapreduce->reducer = reducer;
    mapreduce->user_arg = user_arg;

    *mr = mapreduce;
    return 0;
}

int mr_destroy(mr_t mr) {
    MR_CHECK_NULL(mr);

    free(mr);
    return 0;
}
