#include "mr_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void pair_group_destroy(mr_pair_group_t *group) {
    if (group == NULL) { return; }

    free(group->token);
    for (size_t i = 0; i < group->values_count; i++) {
        free((void *)group->values[i].data);
    }
    free(group->values);
    result_list_destroy(&group->results);

    *group = (mr_pair_group_t){ 0 };

    return;
}

void pair_groups_destroy(mr_pair_groups_t *groups) {
    if (groups == NULL) { return; }

    for (size_t i = 0; i < groups->count; i++) {
        pair_group_destroy(&groups->items[i]);
    }
    free(groups->items);

    *groups = (mr_pair_groups_t){ 0 };

    return;
}

mr_pair_group_t *pair_groups_find(mr_pair_groups_t *groups, const char *token, size_t token_len) {
    if (groups == NULL || token == NULL) { return NULL; }

    for (size_t i = 0; i < groups->count; i++) {
        if (groups->items[i].token_len == token_len && memcmp(groups->items[i].token, token, token_len) == 0) {
            return &groups->items[i];
        }
    }

    return NULL;
}

int pair_group_compare(const void *left, const void *right) {
    const mr_pair_group_t *a = left;
    const mr_pair_group_t *b = right;
    size_t min_len = a->token_len < b->token_len ? a->token_len : b->token_len;
    int cmp = memcmp(a->token, b->token, min_len);

    if (cmp != 0) { return cmp; }
    if (a->token_len < b->token_len) { return -1; }
    if (a->token_len > b->token_len) { return 1; }
    return 0;
}

mr_pair_group_t *pair_groups_push_group(mr_pair_groups_t *groups, mr_pair_item_t *item) {
    if (groups == NULL || item == NULL || item->token == NULL || item->token_len == 0) {
        errno = EINVAL;
        return NULL;
    }

    if (groups->count == groups->capacity) {
        size_t new_capacity = groups->capacity == 0 ? 8 : groups->capacity * 2;

        if (new_capacity < groups->capacity || new_capacity > SIZE_MAX / sizeof(*groups->items)) {
            errno = ENOMEM;
            return NULL;
        }

        mr_pair_group_t *new_items = realloc(groups->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) { return NULL; }

        groups->items = new_items;
        groups->capacity = new_capacity;
    }

    mr_pair_group_t *group = &groups->items[groups->count];
    *group = (mr_pair_group_t){ 0 };
    group->token = item->token;
    group->token_len = item->token_len;
    groups->count++;

    item->token = NULL;
    item->token_len = 0;

    return group;
}

int pair_group_add_value(mr_pair_group_t *group, mr_pair_item_t *item) {
    MR_CHECK_NULL(group);
    MR_CHECK_NULL(item);
    if (item->value_len > 0 && item->value == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (group->values_count == group->values_capacity) {
        size_t new_capacity = group->values_capacity == 0 ? 8 : group->values_capacity * 2;

        if (new_capacity < group->values_capacity || new_capacity > SIZE_MAX / sizeof(*group->values)) {
            errno = ENOMEM;
            return -1;
        }

        mr_value_t *new_values = realloc(group->values, new_capacity * sizeof(*new_values));
        if (new_values == NULL) { return -1; }

        group->values = new_values;
        group->values_capacity = new_capacity;
    }

    mr_value_t *value = &group->values[group->values_count];
    value->data = item->value;
    value->size = item->value_len;
    group->values_count++;

    item->value = NULL;
    item->value_len = 0;

    return 0;
}

int pair_groups_add_pair(mr_pair_groups_t *groups, mr_pair_item_t *pair) {
    MR_CHECK_NULL(groups);
    MR_CHECK_NULL(pair);
    if (pair->token == NULL || pair->token_len == 0 || (pair->value_len > 0 && pair->value == NULL)) {
        errno = EINVAL;
        return -1;
    }

    int created_group = 0;
    mr_pair_group_t *group = pair_groups_find(groups, pair->token, pair->token_len);

    if (group == NULL) {
        group = pair_groups_push_group(groups, pair);
        if (group == NULL) { return -1; }
        created_group = 1;
    }

    if (pair_group_add_value(group, pair) == -1) {
        if (created_group != 0) {
            pair->token = group->token;
            pair->token_len = group->token_len;
            *group = (mr_pair_group_t){ 0 };
            groups->count--;
        }
        return -1;
    }

    if (created_group == 0) {
        free(pair->token);
        pair->token = NULL;
        pair->token_len = 0;
    }

    return 0;
}

int collect_pair_groups(int fd, mr_pair_groups_t *groups) {
    MR_CHECK_NULL(groups);

    for (;;) {
        mr_pair_item_t pair = { 0 };
        int status = read_pair_record(fd, &pair);

        if (status == 0) { return 0; }
        if (status == -1) {
            pair_item_destroy(&pair);
            return -1;
        }

        if (pair_groups_add_pair(groups, &pair) == -1) {
            pair_item_destroy(&pair);
            return -1;
        }

        pair_item_destroy(&pair);
    }
}

typedef struct {
    mr_pair_groups_t *groups;
    mr_reducer_t reducer;
    void *user_arg;
    int log_fd;
    mtx_t *log_lock;
    size_t next_index;
    int result;
    int saved_errno;
    mtx_t lock;
} mr_reducer_worker_context_t;

typedef struct {
    mr_reducer_worker_context_t *context;
    size_t index;
} mr_reducer_worker_arg_t;

static int reducer_worker_main(void *arg) {
    MR_CHECK_NULL(arg);
    mr_reducer_worker_arg_t *worker = arg;
    mr_reducer_worker_context_t *context = worker->context;
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "worker-%zu", worker->index);

    log_message(context->log_fd, context->log_lock, "reducer", thread_name, "THREAD_START", "reducer worker started");

    for (;;) {
        if (mtx_lock(&context->lock) != thrd_success) {
            errno = EIO;
            log_message(context->log_fd, context->log_lock, "reducer", thread_name, "ERROR",
                        "reducer worker lock failed");
            return -1;
        }

        if (context->result == -1 || context->next_index == context->groups->count) {
            int result = context->result;
            mtx_unlock(&context->lock);
            log_message(context->log_fd, context->log_lock, "reducer", thread_name, "THREAD_END",
                        "reducer worker ended");
            return result;
        }

        size_t index = context->next_index;
        context->next_index++;
        mtx_unlock(&context->lock);

        mr_pair_group_t *group = &context->groups->items[index];
        int status = context->reducer(group->token, group->values, group->values_count, reducer_collect_result,
                                      &group->results, context->user_arg);

        if (status == -1) {
            int saved_errno = errno;

            if (mtx_lock(&context->lock) == thrd_success) {
                if (context->result == 0) {
                    context->saved_errno = saved_errno;
                    context->result = -1;
                }
                mtx_unlock(&context->lock);
            }

            errno = saved_errno;
            log_message(context->log_fd, context->log_lock, "reducer", thread_name, "ERROR", "reducer callback failed");
            return -1;
        }
    }
}

int reducer_process_main(mr_t mr, int log_fd) {
    MR_CHECK_NULL(mr);

    mr_pair_groups_t groups = { 0 };
    int result = 0;
    int saved_errno = 0;
    size_t results_produced = 0;
    mtx_t log_lock;

    if (collect_pair_groups(STDIN_FILENO, &groups) == -1) {
        saved_errno = errno;
        pair_groups_destroy(&groups);
        errno = saved_errno;
        return -1;
    }

    if (mtx_init(&log_lock, mtx_plain) != thrd_success) {
        pair_groups_destroy(&groups);
        errno = EIO;
        return -1;
    }

    log_message(log_fd, &log_lock, "reducer", "main", "COUNT", "distinct_tokens=%zu", groups.count);

    qsort(groups.items, groups.count, sizeof(*groups.items), pair_group_compare);

    mr_reducer_worker_context_t context = {
        .groups = &groups,
        .reducer = mr->reducer,
        .user_arg = mr->user_arg,
        .log_fd = log_fd,
        .log_lock = &log_lock,
        .next_index = 0,
        .result = 0,
        .saved_errno = 0,
    };

    if (mtx_init(&context.lock, mtx_plain) != thrd_success) {
        mtx_destroy(&log_lock);
        pair_groups_destroy(&groups);
        errno = EIO;
        return -1;
    }

    thrd_t *worker_threads = malloc(mr->attr.reducer_threads * sizeof(*worker_threads));
    if (worker_threads == NULL) {
        saved_errno = errno;
        mtx_destroy(&context.lock);
        mtx_destroy(&log_lock);
        pair_groups_destroy(&groups);
        errno = saved_errno;
        return -1;
    }

    mr_reducer_worker_arg_t *worker_args = malloc(mr->attr.reducer_threads * sizeof(*worker_args));
    if (worker_args == NULL) {
        saved_errno = errno;
        free(worker_threads);
        mtx_destroy(&context.lock);
        mtx_destroy(&log_lock);
        pair_groups_destroy(&groups);
        errno = saved_errno;
        return -1;
    }

    size_t workers_created = 0;

    for (size_t i = 0; i < mr->attr.reducer_threads; i++) {
        worker_args[i].context = &context;
        worker_args[i].index = i;
        if (thrd_create(&worker_threads[i], reducer_worker_main, &worker_args[i]) != thrd_success) {
            saved_errno = EIO;
            result = -1;
            break;
        }
        workers_created++;
    }

    if (result == -1) {
        if (mtx_lock(&context.lock) == thrd_success) {
            context.result = -1;
            if (context.saved_errno == 0) { context.saved_errno = saved_errno; }
            mtx_unlock(&context.lock);
        }
    }

    int join_failed = 0;
    int worker_failed = 0;

    for (size_t i = 0; i < workers_created; i++) {
        int status = 0;
        if (thrd_join(worker_threads[i], &status) != thrd_success) {
            join_failed = 1;
        } else if (status != 0) {
            worker_failed = 1;
        }
    }

    if (result == 0 && join_failed != 0) {
        saved_errno = EIO;
        result = -1;
    }

    if (result == 0 && (worker_failed != 0 || context.result == -1)) {
        saved_errno = context.saved_errno != 0 ? context.saved_errno : EIO;
        result = -1;
    }

    if (result == 0) {
        for (size_t i = 0; i < groups.count; i++) {
            results_produced += groups.items[i].results.count;
            if (write_result_list(STDOUT_FILENO, &groups.items[i].results) == -1) {
                saved_errno = errno;
                result = -1;
                break;
            }
        }
    }

    if (result == 0) {
        log_message(log_fd, &log_lock, "reducer", "main", "COUNT", "results_produced=%zu", results_produced);
    }

    free(worker_args);
    free(worker_threads);
    pair_groups_destroy(&groups);
    mtx_destroy(&context.lock);
    mtx_destroy(&log_lock);

    if (result == -1) { errno = saved_errno; }
    return result;
}
