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

#include "../src/cpu_count.h"
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
 * @return 0 on success
 *
 * Creates a specified number of processes (default: CPU core count) that
 * each run a busy loop consuming CPU cycles for testing purposes. The
 * number of processes can be specified via argv[1].
 */
int main(int argc, char *argv[]) {
    /* cppcheck-suppress-end constParameter */
    int proc_idx, num_procs;
    pid_t pid = -1;
    configure_signal_handler();
    num_procs = argc == 2 ? atoi(argv[1]) : get_ncpu();
    /* Ensure at least 2 processes to validate -i option in cpulimit */
    num_procs = MAX(num_procs, 2);

    /* Create num_procs-1 child processes (total num_procs processes) */
    for (proc_idx = 1; proc_idx < num_procs; proc_idx++) {
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
    while (!is_quit_flag_set()) {
        volatile int dummy_var;
        for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
            ;
        }
    }

    /*
     * Only the parent writes it: the children share the same terminal, and one
     * newline ends the echo's line as well as several would.
     */
    if (pid > 0) {
        finish_tty_quit_line();
    }
    return 0;
}
