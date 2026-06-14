#include "mr_internal.h"

#include <errno.h>
#include <stdlib.h>

void line_item_destroy(mr_line_item_t *item) {
    if (item == NULL) { return; }

    free((void *)item->file_name);
    free((void *)item->line);
    *item = (mr_line_item_t){ 0 };

    return;
}

int line_queue_init(mr_line_queue_t *queue, size_t capacity) {
    MR_CHECK_NULL(queue);
    if (capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    if ((queue->items = calloc(capacity, sizeof(mr_line_item_t))) == NULL) { return -1; }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = 0;

    if (mtx_init(&queue->lock, mtx_plain) != thrd_success) {
        free(queue->items);
        return -1;
    }

    if (cnd_init(&queue->not_empty) != thrd_success) {
        mtx_destroy(&queue->lock);
        free(queue->items);
        return -1;
    }

    if (cnd_init(&queue->not_full) != thrd_success) {
        cnd_destroy(&queue->not_empty);
        mtx_destroy(&queue->lock);
        free(queue->items);
        return -1;
    }

    return 0;
}

void line_queue_destroy(mr_line_queue_t *queue) {
    if (queue == NULL) { return; }

    if (queue->items != NULL && queue->capacity > 0) {
        for (size_t i = 0; i < queue->count; i++) {
            size_t index = (queue->head + i) % queue->capacity;
            line_item_destroy(&queue->items[index]);
        }
    }

    free(queue->items);
    cnd_destroy(&queue->not_empty);
    cnd_destroy(&queue->not_full);
    mtx_destroy(&queue->lock);

    queue->items = NULL;
    queue->capacity = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = 1;
}

int line_queue_push(mr_line_queue_t *queue, mr_line_item_t *item) {
    MR_CHECK_NULL(queue);
    MR_CHECK_NULL(item);

    if (mtx_lock(&queue->lock) != thrd_success) { return -1; }

    while (queue->count == queue->capacity && !queue->closed) {
        if (cnd_wait(&queue->not_full, &queue->lock) != thrd_success) {
            mtx_unlock(&queue->lock);
            return -1;
        }
    }

    if (queue->closed) {
        mtx_unlock(&queue->lock);
        errno = EPIPE;
        return -1;
    }

    queue->items[queue->tail] = *item;
    *item = (mr_line_item_t){ 0 };
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    cnd_signal(&queue->not_empty);
    mtx_unlock(&queue->lock);

    return 0;
}

int line_queue_pop(mr_line_queue_t *queue, mr_line_item_t *item) {
    MR_CHECK_NULL(queue);
    MR_CHECK_NULL(item);

    if (mtx_lock(&queue->lock) != thrd_success) { return -1; }

    while (queue->count == 0 && !queue->closed) {
        if (cnd_wait(&queue->not_empty, &queue->lock) != thrd_success) {
            mtx_unlock(&queue->lock);
            return -1;
        }
    }

    if (queue->count == 0 && queue->closed) {
        mtx_unlock(&queue->lock);
        return 0;
    }

    *item = queue->items[queue->head];
    queue->items[queue->head] = (mr_line_item_t){ 0 };
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    cnd_signal(&queue->not_full);
    mtx_unlock(&queue->lock);

    return 1;
}

void line_queue_close(mr_line_queue_t *queue) {
    if (queue == NULL) { return; }

    mtx_lock(&queue->lock);
    queue->closed = 1;
    cnd_broadcast(&queue->not_empty);
    cnd_broadcast(&queue->not_full);
    mtx_unlock(&queue->lock);
}
