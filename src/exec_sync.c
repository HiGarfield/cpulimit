/*
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012  Angelo Marletta
 * <angelo dot marletta at gmail dot com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "exec_sync.h"

#include "signal_handler.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * @def EXEC_SYNC_POLL_MS
 * @brief Maximum wait before rechecking the termination flag.
 */
#define EXEC_SYNC_POLL_MS 50

/**
 * @def EXEC_SYNC_QUIT_GRACE_POLLS
 * @brief Polls allowed after a termination request before forced cleanup.
 *
 * A normally starting child still writes its readiness byte and reaches
 * exec after the signal; only a stalled setup is abandoned.
 */
#define EXEC_SYNC_QUIT_GRACE_POLLS 20

/**
 * @brief Kill a child and reap it so it is neither orphaned nor a zombie.
 *
 * @param child_pid PID of the child to kill and reap.
 *
 * waitpid() is retried on EINTR so the child is always reaped.
 */
static void kill_and_reap_child(pid_t child_pid) {
    pid_t wait_result;

    kill(child_pid, SIGKILL);
    do {
        wait_result = waitpid(child_pid, NULL, 0);
    } while (wait_result == -1 && errno == EINTR);
    if (wait_result == -1) {
        perror("waitpid");
    }
}

/**
 * @brief Wait briefly for synchronization data or a termination request.
 *
 * @param sync_read_fd Read end of the exec synchronization pipe.
 * @return 0 when data or EOF is ready, 1 on a termination request, -1 on
 *         a poll error with errno set.
 *
 * @note A bounded wait prevents SA_RESTART from hiding a quit flag while
 *       the child is stalled before writing its readiness byte.
 */
static int wait_for_sync_data(int sync_read_fd) {
    struct pollfd descriptor;
    int idle_polls = 0;
    descriptor.fd = sync_read_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (;;) {
        int ret;
        ret = poll(&descriptor, 1, EXEC_SYNC_POLL_MS);
        if (ret > 0) {
            return 0;
        }
        if (ret < 0 && errno != EINTR) {
            perror("poll");
            return -1;
        }
        /* Let a live child finish its setup before honoring a stalled quit. */
        if (is_quit_flag_set()) {
            if (idle_polls >= EXEC_SYNC_QUIT_GRACE_POLLS - 1) {
                return 1;
            }
            ++idle_polls;
        }
    }
}

/**
 * @brief Wait for the child to finish its exec setup.
 *
 * @param child_pid PID of the forked child.
 * @param sync_read_fd Read end of the exec synchronization pipe.
 * @return 0 on success, -1 on error (the child is killed and reaped).
 *
 * Reads the readiness byte written after setpgid() and handler reset, then
 * drains the pipe to EOF: FD_CLOEXEC closes the write end on a successful
 * exec, and _exit() closes it on failure. Closes sync_read_fd on return.
 *
 * @note Bounded polls recheck the quit flag even though the handlers use
 *       SA_RESTART. A child that finishes setup still follows the normal
 *       signal-forwarding path; a stalled one is killed and reaped after
 *       a short grace period.
 */
int wait_for_child_exec(pid_t child_pid, int sync_read_fd) {
    char sync_byte = '\0';
    ssize_t n_read;
    int poll_result, quit_requested = 0;

    /* Wait for the readiness byte written after setpgid() and handler reset. */
    for (;;) {
        poll_result = wait_for_sync_data(sync_read_fd);
        if (poll_result != 0) {
            quit_requested = poll_result > 0;
            n_read = -1;
            break;
        }
        n_read = read(sync_read_fd, &sync_byte, 1);
        if (n_read >= 0 || errno != EINTR) {
            break;
        }
    }
    if (quit_requested || poll_result < 0) {
        close(sync_read_fd);
        kill_and_reap_child(child_pid);
        return -1;
    }
    if (n_read != 1 || sync_byte != 'A') {
        if (n_read < 0) {
            perror("read sync");
        } else if (n_read == 0) {
            fprintf(stderr, "Synchronization pipe closed before child setup\n");
        } else {
            fprintf(stderr, "Unexpected synchronization value from child\n");
        }
        close(sync_read_fd);
        kill_and_reap_child(child_pid);
        return -1;
    }
    /* Drain to EOF: an early close can raise SIGPIPE in the child. */
    /* stderr may alias the write end when fds 1 and 2 were closed. */
    for (;;) {
        poll_result = wait_for_sync_data(sync_read_fd);
        if (poll_result > 0) {
            quit_requested = 1;
            break;
        }
        if (poll_result < 0) {
            n_read = -1;
            break;
        }
        n_read = read(sync_read_fd, &sync_byte, 1);
        if (n_read > 0) {
            continue;
        }
        if (n_read < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(sync_read_fd);
    if (quit_requested || n_read < 0) {
        if (n_read < 0 && poll_result == 0) {
            perror("read exec-sync");
        }
        kill_and_reap_child(child_pid);
        return -1;
    }
    return 0;
}
