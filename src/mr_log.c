#include "mr_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int format_timestamp(char *buffer, size_t size) {
    MR_CHECK_NULL(buffer);
    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) { return -1; }

    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) { return -1; }

    if (strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    return 0;
}

int log_message(int fd, mtx_t *lock, const char *process, const char *thread, const char *event, const char *fmt, ...) {
    if (fd == -1 || process == NULL || thread == NULL || event == NULL || fmt == NULL) { return 0; }

    char timestamp[32];
    if (format_timestamp(timestamp, sizeof(timestamp)) == -1) { return -1; }

    char message[MR_LOG_MESSAGE_SIZE];
    va_list args;
    va_start(args, fmt);
    int message_len = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (message_len < 0) {
        errno = EIO;
        return -1;
    }

    char line[MR_LOG_LINE_SIZE];
    int line_len = snprintf(line, sizeof(line), "[%s] [%s:%ld] [%s] [%s] %s\n", timestamp, process, (long)getpid(),
                            thread, event, message);
    if (line_len < 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)line_len >= sizeof(line)) {
        errno = EOVERFLOW;
        return -1;
    }

    if (lock != NULL && mtx_lock(lock) != thrd_success) {
        errno = EIO;
        return -1;
    }

    int result = 0;
    int saved_errno = 0;
    if (writen(fd, line, (size_t)line_len) != line_len) {
        saved_errno = errno;
        result = -1;
    }

    if (lock != NULL && mtx_unlock(lock) != thrd_success && result == 0) {
        saved_errno = EIO;
        result = -1;
    }

    if (result == -1) { errno = saved_errno; }
    return result;
}

int log_collector_main(void *arg) {
    MR_CHECK_NULL(arg);

    mr_log_collector_arg_t *collector = arg;
    char buffer[MR_LOG_LINE_SIZE];

    for (;;) {
        ssize_t n_read = read(collector->in_fd, buffer, sizeof(buffer));

        if (n_read == 0) { return 0; }
        if (n_read == -1) {
            if (errno == EINTR) { continue; }
            return -1;
        }

        if (mtx_lock(collector->lock) != thrd_success) {
            errno = EIO;
            return -1;
        }

        int result = 0;
        int saved_errno = 0;
        if (writen(collector->out_fd, buffer, (size_t)n_read) != n_read) {
            saved_errno = errno;
            result = -1;
        }

        if (mtx_unlock(collector->lock) != thrd_success && result == 0) {
            saved_errno = EIO;
            result = -1;
        }

        if (result == -1) {
            errno = saved_errno;
            return -1;
        }
    }
}
