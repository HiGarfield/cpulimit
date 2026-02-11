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

#include "../src/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @brief CPU load generator using fork
 * @param argc Command line argument count
 * @param argv Command line arguments (optional: number of processes)
 * @return Always returns 0 (unreachable)
 * @note This program creates a specified number of processes (default: CPU
 *       core count) that each run an infinite busy loop, consuming CPU cycles
 *       for testing purposes. If a command line argument is provided, it
 *       specifies the number of processes to create. Otherwise, the number of
 *       CPU cores is used.
 */
int main(int argc, const char *argv[]) {
    int i, num_procs;
    volatile int keep_running = 1;
    num_procs = argc == 2 ? atoi(argv[1]) : get_ncpu();
    /* Ensure at least 2 processes to validate -i option in cpulimit */
    num_procs = MAX(num_procs, 2);

    /* Create num_procs-1 child processes (total num_procs processes) */
    for (i = 1; i < num_procs; i++) {
        pid_t pid;
        do {
            pid = fork();
        } while (pid < 0 && errno == EINTR);
        if (pid < 0) { /* fork failed */
            fprintf(stderr, "fork failed\n");
            kill(0, SIGKILL); /* Kill all created children */
            exit(EXIT_FAILURE);
        } else if (pid == 0) { /* Child process (pid == 0) */
            break;             /* Child should not create more processes */
        }
    }

    /* All processes (parent and children) enter infinite loop */
    while (keep_running) {
        volatile int dummy_var;
        for (dummy_var = 0; dummy_var < 1000; dummy_var++) {
            ;
        }
    }

    /* Unreachable code */
    return 0;
}
