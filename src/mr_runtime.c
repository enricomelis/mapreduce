#include "mr_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int mr_start(mr_t mr, const char *input_path, const char *output_path) {
    MR_CHECK_NULL(mr);
    MR_CHECK_NULL(input_path);
    MR_CHECK_NULL(output_path);

    int main_to_mapper[2] = { -1, -1 };
    int mapper_to_reducer[2] = { -1, -1 };
    int reducer_to_main[2] = { -1, -1 };
    int mapper_log_to_main[2] = { -1, -1 };
    int reducer_log_to_main[2] = { -1, -1 };
    int log_file_fd = -1;
    mtx_t log_file_lock;
    int log_file_lock_ready = 0;
    thrd_t mapper_log_thread;
    thrd_t reducer_log_thread;
    int mapper_log_thread_started = 0;
    int reducer_log_thread_started = 0;
    mr_log_collector_arg_t mapper_log_arg = { 0 };
    mr_log_collector_arg_t reducer_log_arg = { 0 };
    pid_t mapper_pid = -1;
    pid_t reducer_pid = -1;
    int mapper_status = 0;
    int reducer_status = 0;
    int result = 0;
    int saved_errno = 0;
    size_t lines_sent = 0;
    size_t results_written = 0;

    log_file_fd = open(mr->attr.log_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_file_fd == -1) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0 && mtx_init(&log_file_lock, mtx_plain) != thrd_success) {
        saved_errno = EIO;
        result = -1;
    } else if (result == 0) {
        log_file_lock_ready = 1;
    }

    if (result == 0 && pipe(main_to_mapper) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created main_to_mapper");
    }

    if (result == 0 && pipe(mapper_to_reducer) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created mapper_to_reducer");
    }

    if (result == 0 && pipe(reducer_to_main) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created reducer_to_main");
    }

    if (result == 0 && pipe(mapper_log_to_main) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created mapper_log_to_main");
    }

    if (result == 0 && pipe(reducer_log_to_main) == -1) {
        saved_errno = errno;
        result = -1;
    } else if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PIPE_CREATE", "created reducer_log_to_main");
    }

    if (result == 0) {
        mapper_pid = fork();

        if (mapper_pid == -1) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (result == 0 && mapper_pid == 0) {
        close_if_open(&log_file_fd);
        if (dup2(main_to_mapper[0], STDIN_FILENO) == -1) { _exit(1); }
        if (dup2(mapper_to_reducer[1], STDOUT_FILENO) == -1) { _exit(1); }

        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);
        close_if_open(&mapper_log_to_main[0]);
        close_if_open(&reducer_log_to_main[0]);
        close_if_open(&reducer_log_to_main[1]);

        log_message(mapper_log_to_main[1], NULL, "mapper", "main", "PROCESS_START", "mapper process started");
        int mapper_status = mapper_process_main(mr, mapper_log_to_main[1]);
        log_message(mapper_log_to_main[1], NULL, "mapper", "main", "PROCESS_END", "mapper process ended status=%d",
                    mapper_status);
        close_if_open(&mapper_log_to_main[1]);
        int exit_status = mapper_status == 0 ? 0 : 1;

        _exit(exit_status);
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PROCESS_CREATE", "mapper pid=%ld", (long)mapper_pid);
    }

    if (result == 0) {
        reducer_pid = fork();

        if (reducer_pid == -1) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (result == 0 && reducer_pid == 0) {
        close_if_open(&log_file_fd);
        if (dup2(mapper_to_reducer[0], STDIN_FILENO) == -1) { _exit(1); }
        if (dup2(reducer_to_main[1], STDOUT_FILENO) == -1) { _exit(1); }

        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);
        close_if_open(&mapper_log_to_main[0]);
        close_if_open(&mapper_log_to_main[1]);
        close_if_open(&reducer_log_to_main[0]);

        log_message(reducer_log_to_main[1], NULL, "reducer", "main", "PROCESS_START", "reducer process started");
        int reducer_status = reducer_process_main(mr, reducer_log_to_main[1]);
        log_message(reducer_log_to_main[1], NULL, "reducer", "main", "PROCESS_END", "reducer process ended status=%d",
                    reducer_status);
        close_if_open(&reducer_log_to_main[1]);
        int exit_status = reducer_status == 0 ? 0 : 1;

        _exit(exit_status);
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "PROCESS_CREATE", "reducer pid=%ld",
                    (long)reducer_pid);
    }

    if (close_if_open(&mapper_log_to_main[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&reducer_log_to_main[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0) {
        mapper_log_arg.in_fd = mapper_log_to_main[0];
        mapper_log_arg.out_fd = log_file_fd;
        mapper_log_arg.lock = &log_file_lock;
        if (thrd_create(&mapper_log_thread, log_collector_main, &mapper_log_arg) != thrd_success) {
            saved_errno = EIO;
            result = -1;
        } else {
            mapper_log_thread_started = 1;
        }
    }

    if (result == 0) {
        reducer_log_arg.in_fd = reducer_log_to_main[0];
        reducer_log_arg.out_fd = log_file_fd;
        reducer_log_arg.lock = &log_file_lock;
        if (thrd_create(&reducer_log_thread, log_collector_main, &reducer_log_arg) != thrd_success) {
            saved_errno = EIO;
            result = -1;
        } else {
            reducer_log_thread_started = 1;
        }
    }

    if (close_if_open(&main_to_mapper[0]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&mapper_to_reducer[0]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&mapper_to_reducer[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (close_if_open(&reducer_to_main[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0 &&
        write_input_path_lines(main_to_mapper[1], input_path, log_file_fd, &log_file_lock, &lines_sent) == -1) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "COUNT", "lines_sent=%zu", lines_sent);
    }

    if (close_if_open(&main_to_mapper[1]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0 &&
        write_reducer_results(reducer_to_main[0], output_path, log_file_fd, &log_file_lock, &results_written) == -1) {
        saved_errno = errno;
        result = -1;
    }

    if (result == 0) {
        log_message(log_file_fd, &log_file_lock, "main", "main", "COUNT", "results_written=%zu", results_written);
    }

    if (close_if_open(&reducer_to_main[0]) == -1 && result == 0) {
        saved_errno = errno;
        result = -1;
    }

    if (result == -1) {
        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);
    }

    if (mapper_pid != -1) {
        if (wait_if_started(&mapper_pid, &mapper_status) == -1 && result == 0) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (reducer_pid != -1) {
        if (wait_if_started(&reducer_pid, &reducer_status) == -1 && result == 0) {
            saved_errno = errno;
            result = -1;
        }
    }

    if (result == 0 && (!WIFEXITED(mapper_status) || WEXITSTATUS(mapper_status) != 0 || !WIFEXITED(reducer_status) ||
                        WEXITSTATUS(reducer_status) != 0)) {
        saved_errno = ECHILD;
        result = -1;
    }

    if (mapper_log_thread_started != 0) {
        int status = 0;
        if (thrd_join(mapper_log_thread, &status) != thrd_success && result == 0) {
            saved_errno = EIO;
            result = -1;
        } else if (status != 0 && result == 0) {
            saved_errno = EIO;
            result = -1;
        }
        mapper_log_thread_started = 0;
    }

    if (reducer_log_thread_started != 0) {
        int status = 0;
        if (thrd_join(reducer_log_thread, &status) != thrd_success && result == 0) {
            saved_errno = EIO;
            result = -1;
        } else if (status != 0 && result == 0) {
            saved_errno = EIO;
            result = -1;
        }
        reducer_log_thread_started = 0;
    }

    close_if_open(&main_to_mapper[0]);
    close_if_open(&main_to_mapper[1]);
    close_if_open(&mapper_to_reducer[0]);
    close_if_open(&mapper_to_reducer[1]);
    close_if_open(&reducer_to_main[0]);
    close_if_open(&reducer_to_main[1]);
    close_if_open(&mapper_log_to_main[0]);
    close_if_open(&mapper_log_to_main[1]);
    close_if_open(&reducer_log_to_main[0]);
    close_if_open(&reducer_log_to_main[1]);
    wait_if_started(&mapper_pid, NULL);
    wait_if_started(&reducer_pid, NULL);

    if (log_file_fd != -1) {
        if (result == -1) {
            log_message(log_file_fd, log_file_lock_ready != 0 ? &log_file_lock : NULL, "main", "main", "ERROR",
                        "mr_start failed errno=%d", saved_errno);
        }
        close_if_open(&log_file_fd);
    }

    if (log_file_lock_ready != 0) { mtx_destroy(&log_file_lock); }

    if (result == -1) { errno = saved_errno; }
    return result;
}
