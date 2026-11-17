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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

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
 * @note The reads run with SA_RESTART, so a termination signal received
 *       during exec setup does not end this wait.
 */
int wait_for_child_exec(pid_t child_pid, int sync_read_fd) {
    char sync_byte;
    ssize_t n_read;

    /* Wait for the readiness byte written after setpgid() and handler reset. */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read < 0 && errno == EINTR);
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
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read > 0 || (n_read < 0 && errno == EINTR));
    close(sync_read_fd);
    if (n_read < 0) {
        perror("read exec-sync");
        kill_and_reap_child(child_pid);
        return -1;
    }
    return 0;
}
