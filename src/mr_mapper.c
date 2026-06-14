#include "mr_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mapper_emit_pair(const char *token, const void *value, size_t value_size, void *emit_arg) {
    MR_CHECK_NULL(emit_arg);
    mr_mapper_context_t *context = emit_arg;

    if (!is_valid_token(token) || (value_size > 0 && value == NULL)) {
        errno = EINVAL;
        return -1;
    }

    size_t token_len = strlen(token);
    if (token_len > INT_MAX || value_size > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    mr_pair_header_t header = {
        .token_len = (int)token_len,
        .value_len = (int)value_size,
    };

    if (mtx_lock(&context->pipe_emit_lock) != thrd_success) {
        errno = EIO;
        return -1;
    }

    int saved_errno = 0;

    if (writen(context->out_fd, &header, sizeof(header)) != (ssize_t)sizeof(header)) {
        saved_errno = errno;
        mtx_unlock(&context->pipe_emit_lock);
        errno = saved_errno;
        return -1;
    }

    if (writen(context->out_fd, token, token_len) != (ssize_t)token_len) {
        saved_errno = errno;
        mtx_unlock(&context->pipe_emit_lock);
        errno = saved_errno;
        return -1;
    }

    if (value_size > 0 && writen(context->out_fd, value, value_size) != (ssize_t)value_size) {
        saved_errno = errno;
        mtx_unlock(&context->pipe_emit_lock);
        errno = saved_errno;
        return -1;
    }

    context->pairs_produced++;
    mtx_unlock(&context->pipe_emit_lock);

    return 0;
}

static int mapper_reader_main(void *arg) {
    MR_CHECK_NULL(arg);
    mr_mapper_context_t *context = arg;

    log_message(context->log_fd, &context->log_lock, "mapper", "reader", "THREAD_START", "mapper reader started");

    mr_line_item_t item = { 0 };

    for (;;) {
        item = (mr_line_item_t){ 0 };
        int queue_status = read_line_record(STDIN_FILENO, &item);

        if (queue_status == -1) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", "reader", "ERROR", "mapper reader failed");
            return -1;
        }

        if (queue_status == 0) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", "reader", "THREAD_END", "mapper reader ended");
            return 0;
        }

        if (queue_status == 1) {
            if (line_queue_push(&context->queue, &item) == -1) {
                line_item_destroy(&item);
                line_queue_close(&context->queue);
                log_message(context->log_fd, &context->log_lock, "mapper", "reader", "ERROR",
                            "mapper reader queue push failed");
                return -1;
            }
        }
    }
    return 0;
}

int mapper_worker_main(void *arg) {
    MR_CHECK_NULL(arg);
    mr_mapper_worker_arg_t *worker = arg;
    mr_mapper_context_t *context = worker->context;
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "worker-%zu", worker->index);

    log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "THREAD_START", "mapper worker started");

    for (;;) {
        mr_line_item_t item = { 0 };
        int queue_status = line_queue_pop(&context->queue, &item);

        if (queue_status == -1) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "ERROR",
                        "mapper worker queue pop failed");
            return -1;
        }

        if (queue_status == 0) {
            log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "THREAD_END",
                        "mapper worker ended");
            return 0;
        }

        mr_file_line_t line = { .file_name = item.file_name,
                                .file_name_len = item.file_name_len,
                                .line = item.line,
                                .line_len = item.line_len,
                                .line_number = item.line_number };

        int mapper_status = context->mapper(&line, mapper_emit_pair, context, context->user_arg);

        line_item_destroy(&item);

        if (mapper_status == -1) {
            line_queue_close(&context->queue);
            log_message(context->log_fd, &context->log_lock, "mapper", thread_name, "ERROR", "mapper callback failed");
            return -1;
        }
    }

    return 0;
}

int mapper_process_main(mr_t mr, int log_fd) {
    MR_CHECK_NULL(mr);

    mr_mapper_context_t context = { 0 };
    context.mapper = mr->mapper;
    context.user_arg = mr->user_arg;
    context.out_fd = STDOUT_FILENO;
    context.log_fd = log_fd;

    if (line_queue_init(&context.queue, mr->attr.queue_size) == -1) { return -1; }

    if (mtx_init(&context.pipe_emit_lock, mtx_plain) != thrd_success) {
        line_queue_destroy(&context.queue);
        return -1;
    }

    if (mtx_init(&context.log_lock, mtx_plain) != thrd_success) {
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    thrd_t *worker_threads = malloc(mr->attr.mapper_threads * sizeof(*worker_threads));
    if (worker_threads == NULL) {
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    mr_mapper_worker_arg_t *worker_args = malloc(mr->attr.mapper_threads * sizeof(*worker_args));
    if (worker_args == NULL) {
        free(worker_threads);
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    size_t workers_created = 0;
    int result = 0;

    for (size_t i = 0; i < mr->attr.mapper_threads; i++) {
        worker_args[i].context = &context;
        worker_args[i].index = i;
        if (thrd_create(&worker_threads[i], mapper_worker_main, &worker_args[i]) != thrd_success) {
            result = -1;
            line_queue_close(&context.queue);
            break;
        }
        workers_created++;
    }

    int status;
    if (result == -1) {
        for (size_t i = 0; i < workers_created; i++) {
            if (thrd_join(worker_threads[i], &status) != thrd_success) { result = -1; }
        }

        free(worker_args);
        free(worker_threads);
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    thrd_t reader_thread;
    if (thrd_create(&reader_thread, mapper_reader_main, &context) != thrd_success) {
        line_queue_close(&context.queue);

        for (size_t i = 0; i < workers_created; i++) {
            if (thrd_join(worker_threads[i], &status) != thrd_success) { result = -1; }
        }

        free(worker_args);
        free(worker_threads);
        mtx_destroy(&context.log_lock);
        mtx_destroy(&context.pipe_emit_lock);
        line_queue_destroy(&context.queue);
        return -1;
    }

    if (thrd_join(reader_thread, &status) != thrd_success || status != 0) { result = -1; }

    for (size_t i = 0; i < workers_created; i++) {
        if (thrd_join(worker_threads[i], &status) != thrd_success || status != 0) { result = -1; }
    }

    log_message(context.log_fd, &context.log_lock, "mapper", "main", "COUNT", "pairs_produced=%zu",
                context.pairs_produced);

    free(worker_args);
    free(worker_threads);
    mtx_destroy(&context.log_lock);
    mtx_destroy(&context.pipe_emit_lock);
    line_queue_destroy(&context.queue);

    return result;
}
