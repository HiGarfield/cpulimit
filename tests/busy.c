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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * @brief Busy loop thread function
 * @param arg Thread argument (unused)
 * @return NULL pointer
 * @note This function runs an infinite busy loop to consume CPU cycles
 */
static void *busy_loop(void *arg) {
    (void)arg;
    while (!is_quit_flag_set()) {
        volatile int dummy_var;
        for (dummy_var = 0; dummy_var < 1000; dummy_var++) {
            ;
        }
    }
    return NULL;
}

/**
 * @brief Main function for CPU load generator
 * @param argc Argument count
 * @param argv Argument vector
 * @return 0 on success, exits with failure on error
 * @note This program creates multiple threads to generate CPU load for
 *  testing cpulimit functionality. Number of threads can be specified via
 *  command line argument.
 */
int main(int argc, const char *argv[]) {
    int thread_idx, num_threads;
    pthread_attr_t attr;

    configure_signal_handler();

    num_threads = argc == 2 ? atoi(argv[1]) : get_ncpu();
    num_threads = MAX(num_threads, 1);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    for (thread_idx = 1; thread_idx < num_threads; thread_idx++) {
        pthread_t tid;
        if (pthread_create(&tid, &attr, busy_loop, NULL) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    pthread_attr_destroy(&attr);

    busy_loop(NULL);

    if (is_quit_flag_set() && is_terminated_by_tty() && isatty(STDIN_FILENO) &&
        isatty(STDOUT_FILENO)) {
        ssize_t write_result = write(STDOUT_FILENO, "\n", 1);
        (void)write_result;
    }

    return 0;
}
