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
     * Drain the sync pipe until EOF to confirm the child has closed its
     * write end.  On a successful exec, FD_CLOEXEC closes the write end
     * automatically; on exec failure the child closes it explicitly
     * before _exit().  Only EOF (n_read == 0) guarantees the write end
     * is closed and the child will not write again.
     *
     * Reading just one byte and then closing the read end is unsafe:
     * if the caller has closed fds 1 and 2 before this function is
     * called, pipe() assigns those fds to the sync pipe, causing the
     * child's FILE *stderr (fd 2) to alias the sync pipe write end.
     * perror() in the exec-failure path then makes multiple write()
     * calls to fd 2.  Closing the read end after only one byte leaves
     * the write end still open; the child's next write() receives
     * SIGPIPE (default action: terminate), so the child dies from a
     * signal instead of calling _exit(127), producing exit code 141
     * (128+SIGPIPE) rather than 127.
     *
     * Draining until EOF keeps the read end open for all of the child's
     * writes, eliminating the SIGPIPE race.  It also eliminates the race
     * where a signal (e.g. SIGTERM) is sent while the child is still in
     * the middle of exec setup, which is critical under tools such as
     * valgrind that intercept execve and may not handle signals safely
     * during their exec interception phase.
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
