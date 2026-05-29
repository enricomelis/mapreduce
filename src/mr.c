#include "mr.h"
#include <errno.h>
#include <stdlib.h>
#include <threads.h>

struct mr {
    mr_attr_t attr;
    mr_mapper_t mapper;
    mr_reducer_t reducer;
    void *user_arg;
};

typedef struct {
    char *file_name;
    size_t file_name_len;
    unsigned long line_number;
    char *line;
    size_t line_len;
} mr_line_item_t;

/* coda per pattern produttore-consumatore nei thread del processo mapper */
typedef struct {
    mr_line_item_t *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int closed;
    mtx_t lock;
    cnd_t not_empty;
    cnd_t not_full;
} mr_line_queue_t;

static int line_queue_init(mr_line_queue_t *queue, size_t capacity);
static void line_queue_destroy(mr_line_queue_t *queue);
static int line_queue_push(mr_line_queue_t *queue, mr_line_item_t item);
static int line_queue_pop(mr_line_queue_t *queue, mr_line_item_t *item);
static void line_queue_close(mr_line_queue_t *queue);