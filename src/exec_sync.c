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

int wait_for_child_exec(pid_t child_pid, int sync_read_fd) {
    /* Synchronization byte from child */
    char sync_byte;
    /* Bytes read from pipe */
    ssize_t n_read;

    /*
     * Block until child signals readiness by writing to pipe.
     * This ensures child has completed setpgid() before parent continues.
     */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    if (n_read != 1 || sync_byte != 'A') {
        /* Return value of waitpid in error path */
        pid_t wait_result;
        if (n_read < 0) {
            perror("read sync");
        } else if (n_read == 0) {
            fprintf(stderr, "Synchronization pipe closed before child setup\n");
        } else {
            fprintf(stderr, "Unexpected synchronization value from child\n");
        }
        close(sync_read_fd);
        /*
         * Kill child to prevent it from becoming an orphan, then reap
         * it to prevent zombie. SIGKILL is used because the child may
         * have failed in an unknown state and cannot be trusted to
         * respond to SIGTERM. SIGKILL cannot be caught or ignored, so
         * the subsequent blocking waitpid() will complete quickly.
         */
        kill(child_pid, SIGKILL);
        /*
         * Robustly reap the child: retry waitpid() on EINTR so that
         * a signal delivered to the parent during the wait does not
         * leave the child as a zombie. For any other error, log it and
         * proceed with the fatal exit.
         */
        do {
            wait_result = waitpid(child_pid, NULL, 0);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == -1) {
            perror("waitpid");
        }
        return -1;
    }
    /*
     * Drain the sync pipe until EOF, the only event that proves the child has
     * closed its write end: a successful exec closes it through FD_CLOEXEC,
     * and an exec failure closes it explicitly before _exit().
     *
     * Stopping after one byte would be unsafe.  If the caller has closed fds
     * 1 and 2, pipe() hands those numbers to the sync pipe, so the child's
     * stderr aliases the write end; the writes perror() makes on the
     * exec-failure path would then hit a pipe whose read end is already
     * closed, raising SIGPIPE and killing the child with 141 instead of
     * letting it report 127.  Draining to EOF keeps the read end open for
     * every write, and also removes the race where a signal arrives while the
     * child is still inside exec setup -- which matters under tools such as
     * valgrind that intercept execve.
     */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read > 0 || (n_read < 0 && errno == EINTR));
    close(sync_read_fd);
    if (n_read < 0) {
        pid_t wait_result;
        perror("read exec-sync");
        kill(child_pid, SIGKILL);
        do {
            wait_result = waitpid(child_pid, NULL, 0);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == -1) {
            perror("waitpid");
        }
        return -1;
    }
    return 0;
}
