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

#include "../src/signal_handler.h"
#include "../src/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/* cppcheck-suppress-begin constParameter */
/**
 * @brief CPU load generator using fork
 * @param argc Command line argument count
 * @param argv Command line arguments (optional: number of processes)
 * @return 0 on normal shutdown
 * @note This program creates a specified number of processes (default: CPU
 *       core count) that each run an infinite busy loop, consuming CPU cycles
 *       for testing purposes. If a command line argument is provided, it
 *       specifies the number of processes to create. Otherwise, the number
 *       of CPU cores is used.
 */
int main(int argc, char *argv[]) {
    /* cppcheck-suppress-end constParameter */
    int proc_idx, num_procs, num_children, kill_idx;
    pid_t pid = -1;
    pid_t *child_pids;
    configure_signal_handler();
    num_procs = argc == 2 ? atoi(argv[1]) : get_ncpu();
    /* Ensure at least 2 processes to validate -i option in cpulimit */
    num_procs = MAX(num_procs, 2);

    /*
     * Allocate storage for child PIDs so we can kill only the children
     * we actually forked on failure, rather than the whole process group.
     */
    child_pids = (pid_t *)malloc((size_t)(num_procs - 1) * sizeof(pid_t));
    if (child_pids == NULL) {
        fprintf(stderr, "malloc failed: out of memory\n");
        exit(EXIT_FAILURE);
    }
    num_children = 0;

    /* Create num_procs-1 child processes (total num_procs processes) */
    for (proc_idx = 1; proc_idx < num_procs; proc_idx++) {
        do {
            pid = fork();
        } while (pid < 0 && errno == EINTR);
        if (pid < 0) { /* fork failed */
            fprintf(stderr, "fork failed\n");
            /*
             * Kill only the children forked by this process.
             * kill(0, SIGKILL) would also kill the test runner
             * if we share a process group with it.
             */
            for (kill_idx = 0; kill_idx < num_children; kill_idx++) {
                kill(child_pids[kill_idx], SIGKILL);
            }
            free(child_pids);
            exit(EXIT_FAILURE);
        } else if (pid == 0) { /* Child process (pid == 0) */
            break;             /* Child should not create more processes */
        }
        child_pids[num_children++] = pid;
    }
    free(child_pids);
    child_pids = NULL;

    /* All processes (parent and children) enter infinite loop */
    while (!is_quit_flag_set()) {
        volatile int dummy_var;
        for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
            ;
        }
    }

    if (pid > 0 && is_quit_flag_set() && is_terminated_by_tty() &&
        isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        if (write(STDOUT_FILENO, "\n", 1) != 1) {
            /* non-fatal: cosmetic newline at shutdown */
        }
    }
    return 0;
}
