#include "mr_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *full_path;
    char *file_name;
} mr_input_file_t;

int write_file_lines(int out_fd, const char *path, const char *file_name, int log_fd, mtx_t *log_lock,
                     size_t *lines_sent) {
    MR_CHECK_NULL(path);
    MR_CHECK_NULL(file_name);

    log_message(log_fd, log_lock, "main", "main", "FILE_OPEN", "input path=%s", path);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { return -1; }

    mr_line_item_t item = { 0 };

    unsigned long line_num = 1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n_read = 0;
    size_t file_name_len = strlen(file_name);

    while ((n_read = getline(&line, &capacity, fp)) != -1) {
        size_t line_len = (size_t)n_read;
        if (line_len > 0 && line[line_len - 1] == '\n') { line_len--; }

        item.file_name = file_name;
        item.file_name_len = file_name_len;
        item.line = line;
        item.line_len = line_len;
        item.line_number = line_num;

        line_num++;

        if (write_line_record(out_fd, &item) == -1) {
            int saved_errno = errno;
            free(line);
            fclose(fp);
            errno = saved_errno;
            return -1;
        }

        if (lines_sent != NULL) { (*lines_sent)++; }
    }

    if (ferror(fp)) {
        int saved_errno = errno != 0 ? errno : EIO;
        free(line);
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    free(line);
    if (fclose(fp) == EOF) { return -1; }
    log_message(log_fd, log_lock, "main", "main", "FILE_CLOSE", "input path=%s", path);

    return 0;
}

static void input_files_destroy(mr_input_file_t *files, size_t count) {
    if (files == NULL) { return; }

    for (size_t i = 0; i < count; i++) {
        free(files[i].full_path);
        free(files[i].file_name);
    }

    free(files);
}

static int input_file_compare(const void *left, const void *right) {
    const mr_input_file_t *a = left;
    const mr_input_file_t *b = right;

    return strcmp(a->file_name, b->file_name);
}

static const char *input_path_basename(const char *path) {
    const char *slash = strrchr(path, '/');

    if (slash == NULL) { return path; }
    return slash + 1;
}

static int build_full_path(const char *directory, const char *file_name, char **out) {
    MR_CHECK_NULL(directory);
    MR_CHECK_NULL(file_name);
    MR_CHECK_NULL(out);

    size_t directory_len = strlen(directory);
    size_t file_name_len = strlen(file_name);
    int add_slash = directory_len > 0 && directory[directory_len - 1] != '/';

    if (directory_len > SIZE_MAX - file_name_len - (size_t)add_slash - 1) {
        errno = EOVERFLOW;
        return -1;
    }

    size_t full_path_len = directory_len + (size_t)add_slash + file_name_len;
    char *full_path = malloc(full_path_len + 1);
    if (full_path == NULL) { return -1; }

    memcpy(full_path, directory, directory_len);
    if (add_slash) { full_path[directory_len] = '/'; }
    memcpy(full_path + directory_len + (size_t)add_slash, file_name, file_name_len);
    full_path[full_path_len] = '\0';

    *out = full_path;
    return 0;
}

static int input_files_push(mr_input_file_t **files, size_t *count, size_t *capacity, const char *full_path,
                            const char *file_name) {
    MR_CHECK_NULL(files);
    MR_CHECK_NULL(count);
    MR_CHECK_NULL(capacity);
    MR_CHECK_NULL(full_path);
    MR_CHECK_NULL(file_name);

    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(**files)) {
            errno = EOVERFLOW;
            return -1;
        }

        mr_input_file_t *new_files = realloc(*files, new_capacity * sizeof(*new_files));
        if (new_files == NULL) { return -1; }

        *files = new_files;
        *capacity = new_capacity;
    }

    char *full_path_copy = strdup(full_path);
    if (full_path_copy == NULL) { return -1; }

    char *file_name_copy = strdup(file_name);
    if (file_name_copy == NULL) {
        free(full_path_copy);
        return -1;
    }

    (*files)[*count].full_path = full_path_copy;
    (*files)[*count].file_name = file_name_copy;
    (*count)++;

    return 0;
}

static int write_directory_lines(int out_fd, const char *input_path, int log_fd, mtx_t *log_lock, size_t *lines_sent) {
    DIR *dir = opendir(input_path);
    if (dir == NULL) { return -1; }

    mr_input_file_t *files = NULL;
    size_t count = 0;
    size_t capacity = 0;

    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) { continue; }

        char *full_path = NULL;
        if (build_full_path(input_path, entry->d_name, &full_path) == -1) {
            int saved_errno = errno;
            input_files_destroy(files, count);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        struct stat st;
        if (stat(full_path, &st) == -1) {
            int saved_errno = errno;
            free(full_path);
            input_files_destroy(files, count);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        if (S_ISREG(st.st_mode) && input_files_push(&files, &count, &capacity, full_path, entry->d_name) == -1) {
            int saved_errno = errno;
            free(full_path);
            input_files_destroy(files, count);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        free(full_path);
        errno = 0;
    }

    if (errno != 0) {
        int saved_errno = errno;
        input_files_destroy(files, count);
        closedir(dir);
        errno = saved_errno;
        return -1;
    }

    if (closedir(dir) == -1) {
        int saved_errno = errno;
        input_files_destroy(files, count);
        errno = saved_errno;
        return -1;
    }

    qsort(files, count, sizeof(*files), input_file_compare);

    for (size_t i = 0; i < count; i++) {
        if (write_file_lines(out_fd, files[i].full_path, files[i].file_name, log_fd, log_lock, lines_sent) == -1) {
            int saved_errno = errno;
            input_files_destroy(files, count);
            errno = saved_errno;
            return -1;
        }
    }

    input_files_destroy(files, count);
    return 0;
}

int write_input_path_lines(int out_fd, const char *input_path, int log_fd, mtx_t *log_lock, size_t *lines_sent) {
    MR_CHECK_NULL(input_path);
    if (lines_sent != NULL) { *lines_sent = 0; }

    struct stat st;
    if (stat(input_path, &st) == -1) { return -1; }

    if (S_ISREG(st.st_mode)) {
        return write_file_lines(out_fd, input_path, input_path_basename(input_path), log_fd, log_lock, lines_sent);
    }

    if (S_ISDIR(st.st_mode)) { return write_directory_lines(out_fd, input_path, log_fd, log_lock, lines_sent); }

    errno = EINVAL;
    return -1;
}
