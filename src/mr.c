#include "mr.h"
#include <errno.h>
#include <stdlib.h>
#include <threads.h>

#define MR_CHECK_NULL(attr) \
    do { \
        if((attr) == NULL){ \
            errno = EINVAL; \
            return -1; \
        } \
    } while(0)

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

/* ====================================================================== */
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

/* 
 * inizializzazione della coda: 
 * 1. controllo input invalidi
 * 2. allocazione memoria dinamica inizializzata
 * 3. campi della struct
 * 4. strutture di sincronizzazione con eventuale distruzione dei precedenti
*/
static int line_queue_init(mr_line_queue_t *queue, size_t capacity) {
    if (capacity == 0 || queue == NULL) { errno = EINVAL; return -1; }

    if ((queue->items = calloc(capacity, sizeof(mr_line_item_t))) == NULL) { return -1; }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = 0;

    if(mtx_init(&queue->lock, mtx_plain) != thrd_success){ 
        free(queue->items); 
        return -1; 
    }

    if(cnd_init(&queue->not_empty) != thrd_success){ 
        mtx_destroy(&queue->lock); 
        free(queue->items); 
        return -1; 
    }

    if(cnd_init(&queue->not_full) != thrd_success){ 
        cnd_destroy(&queue->not_empty); 
        mtx_destroy(&queue->lock); 
        free(queue->items); 
        return -1; 
    }

    return 0;
}

/* distruzione della coda
 * 1. controllo input invalidi
 * 2. free circolare sugli elementi validi della coda
 * 3. distruzione delle strutture e azzeramento dei campi
 */
static void line_queue_destroy(mr_line_queue_t *queue){
    if(queue == NULL){ return; }

    if(queue->items != NULL && queue->capacity > 0){
        for(size_t i = 0; i < queue->count; i++){
            size_t index = (queue->head + i) % queue->capacity;
            free(queue->items[index].file_name);
            free(queue->items[index].line);
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

static int line_queue_push(mr_line_queue_t *queue, mr_line_item_t item){
    if(queue == NULL){ errno = EINVAL; return -1; }

    if(mtx_lock(&queue->lock) != thrd_success){ return -1; }

    while(queue->count == queue->capacity && !queue->closed){
        if(cnd_wait(&queue->not_full, &queue->lock) != thrd_success){
            mtx_unlock(&queue->lock);
            return -1;
        }
    }

    if(queue->closed){
        mtx_unlock(&queue->lock);
        errno = EPIPE;
        return -1;
    }

    queue->items[queue->tail] = item;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    cnd_signal(&queue->not_empty);
    mtx_unlock(&queue->lock);

    return 0;
}

/* valori di return
* `-1`: errore
* `1`: item estratto
* `0`: coda chiusa e vuota
*/
static int line_queue_pop(mr_line_queue_t *queue, mr_line_item_t *item){
    if(queue == NULL || item == NULL){ errno = EINVAL; return -1; }

    if(mtx_lock(&queue->lock) != thrd_success){ return -1; }

    while(queue->count == 0 && !queue->closed){
        if(cnd_wait(&queue->not_empty, &queue->lock) != thrd_success){
            mtx_unlock(&queue->lock);
            return -1;
        }
    }

    if(queue->count == 0 && queue->closed){
        mtx_unlock(&queue->lock);
        return 0;
    }

    *item = queue->items[queue->head];
    queue->items[queue->head] = (mr_line_item_t){0};
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    cnd_signal(&queue->not_full);
    mtx_unlock(&queue->lock);

    return 1;
}

static void line_queue_close(mr_line_queue_t *queue){
    if(queue == NULL){ return; }

    mtx_lock(&queue->lock);
    queue->closed = 1;
    cnd_broadcast(&queue->not_empty);
    cnd_broadcast(&queue->not_full);
    mtx_unlock(&queue->lock);
}

/* ====================================================================== */

int mr_attr_init(mr_attr_t *attr){
    if(attr == NULL){
        errno = EINVAL;
        return -1;
    }
    
    attr->mapper_threads = 1;
    attr->reducer_threads = 1;
    attr->queue_size = 64;
    attr->log_file = NULL;
    
    return 0;
}

int mr_attr_destroy(mr_attr_t *attr){
    if(attr == NULL){
        errno = EINVAL;
        return -1;
    }

    attr->mapper_threads = 0;
    attr->reducer_threads = 0;
    attr->queue_size = 0;
    attr->log_file = NULL;

    return 0;
}

int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n){
    MR_CHECK_NULL(attr);
    if(n == 0){
        errno = EINVAL;
        return -1;
    }

    attr->mapper_threads = n;
    return 0;
}

int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n){
    MR_CHECK_NULL(attr);
    if(n == 0){
        errno = EINVAL;
        return -1;
    }

    attr->reducer_threads = n;
    return 0;
}

int mr_attr_set_queue_size(mr_attr_t *attr, size_t n){
    MR_CHECK_NULL(attr);
    if(n == 0){
        errno = EINVAL;
        return -1;
    }

    attr->queue_size = n;
    return 0;
}

int mr_attr_set_log_file(mr_attr_t *attr, const char *path){
    MR_CHECK_NULL(attr);

    attr->log_file = path;
    return 0;
}

int mr_create(mr_t *mr, const mr_attr_t *attr, mr_mapper_t mapper, mr_reducer_t reducer, void *user_arg){
    MR_CHECK_NULL(mr);
    MR_CHECK_NULL(attr);
    MR_CHECK_NULL(mapper);
    MR_CHECK_NULL(reducer);

    *mr = NULL;

    if(attr->mapper_threads == 0 || attr->reducer_threads == 0 || attr->queue_size == 0){
        errno = EINVAL;
        return -1;
    }

    mr_t mapreduce = malloc(sizeof(*mapreduce));
    if(mapreduce == NULL){
        return -1;
    }

    mapreduce->attr = *attr;
    mapreduce->mapper = mapper;
    mapreduce->reducer = reducer;
    mapreduce->user_arg = user_arg;

    *mr = mapreduce;
    return 0;
}

int mr_destroy(mr_t mr){
    MR_CHECK_NULL(mr);

    free(mr);
    return 0;
}
