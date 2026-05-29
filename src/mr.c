#include "mr.h"
#include <errno.h>

/* stato interno del framework */
struct mr {
    mr_attr_t attr;
    mr_mapper_t mapper;
    mr_reducer_t reducer;
    void *user_arg;
};

/* container di righe */
typedef struct {
    char *file_name;
    size_t file_name_len;
    unsigned long line_numer;
    char *line;
    size_t line_len;
} mr_line_item_t;