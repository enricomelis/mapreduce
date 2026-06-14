#include "mr_internal.h"

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

ssize_t readn(int fd, void *buf, size_t n) {
    char *p = buf;
    size_t total = 0;

    while (total < n) {
        ssize_t act = read(fd, p + total, n - total);

        if (act > 0) {
            total += (size_t)act;
            continue;
        }

        if (act == 0) {
            if (total == 0) { return 0; }

            errno = EPROTO;
            return -1;
        }

        if (errno == EINTR) { continue; }

        return -1;
    }

    return (ssize_t)total;
}

ssize_t writen(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t total = 0;

    while (total < n) {
        ssize_t act = write(fd, p + total, n - total);

        if (act > 0) {
            total += (size_t)act;
            continue;
        }

        if (act == 0) {
            errno = EIO;
            return -1;
        }

        if (errno == EINTR) { continue; }

        return -1;
    }

    return (ssize_t)total;
}

int close_if_open(int *fd) {
    if (fd == NULL || *fd == -1) { return 0; }

    int current = *fd;
    *fd = -1;
    return close(current);
}

int wait_if_started(pid_t *pid, int *status) {
    if (pid == NULL || *pid == -1) { return 0; }

    for (;;) {
        if (waitpid(*pid, status, 0) != -1) {
            *pid = -1;
            return 0;
        }

        if (errno == EINTR) { continue; }
        return -1;
    }
}
