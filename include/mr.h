#ifndef MR_H
#define MR_H

#include <stddef.h>

/* Handle opaco di una elaborazione. */
typedef struct mr *mr_t;

/*
 * Attributi di configurazione del framework.
 *
 * mapper_threads : numero di thread usati nel processo mapper.
 * reducer_threads: numero di thread usati nel processo reducer.
 * queue_size     : dimensione delle code interne usate dal framework.
 * log_file       : nome del file di log, oppure NULL per usare il nome di default.
 */
typedef struct {
    size_t mapper_threads;
    size_t reducer_threads;
    size_t queue_size;
    const char *log_file;
} mr_attr_t;

/*
 * Riga logica di un file, vista dalla funzione mapper.
 */
typedef struct {
    const char *file_name;
    size_t file_name_len;
    unsigned long line_number;
    const char *line;
    size_t line_len;
} mr_file_line_t;

/*
 * Valore opaco associato a un token.
 * Il framework non interpreta il contenuto di data.
 * Se size vale 0, data può essere NULL.
 */
typedef struct {
    const void *data;
    size_t size;
} mr_value_t;

/*
 * Funzione usata dal mapper per emettere una coppia <token, valore>.
 * token deve essere una stringa C valida, terminata da '\0',
 * composta soltanto da caratteri alfanumerici ASCII.
 */
typedef int (*mr_emit_pair_t)(const char *token, const void *value, size_t value_size,
                              void *emit_arg);

/*
 * Funzione usata dal reducer per emettere un risultato finale.
 */
typedef int (*mr_emit_result_t)(const char *token, const void *result, size_t result_size,
                                void *emit_arg);

/*
 * Funzione mapper fornita dal programma utente.
 */
typedef int (*mr_mapper_t)(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg,
                           void *user_arg);

/*
 * Funzione reducer fornita dal programma utente.
 */
typedef int (*mr_reducer_t)(const char *token, const mr_value_t *values, size_t values_count,
                            mr_emit_result_t emit, void *emit_arg, void *user_arg);

int mr_attr_init(mr_attr_t *attr);
int mr_attr_destroy(mr_attr_t *attr);

int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_queue_size(mr_attr_t *attr, size_t n);
int mr_attr_set_log_file(mr_attr_t *attr, const char *path);

int mr_create(mr_t *mr, const mr_attr_t *attr, mr_mapper_t mapper, mr_reducer_t reducer,
              void *user_arg);

int mr_start(mr_t mr, const char *input_path, const char *output_path);
int mr_destroy(mr_t mr);

#endif
