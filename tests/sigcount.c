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

/*
 * Test helper: count SIGINT deliveries and report the count as exit status.
 *
 * Used by test_limiter_run_command_mode_forwards_signal_once() to prove that
 * interrupting the limiter once delivers exactly one SIGINT to the command.
 * The optional argv[1] is a file created once the handler is installed, so
 * the driving test can synchronise instead of guessing a delay.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** @brief How many 10 ms slots to wait for the first SIGINT (20 s). */
#define SIGCOUNT_WAIT_SLOTS 2000

/** @brief How many 10 ms slots to linger afterwards, so a duplicate shows. */
#define SIGCOUNT_LINGER_SLOTS 150

/** @brief Number of SIGINTs received so far. */
static volatile sig_atomic_t sigint_count = 0;

/**
 * @brief Record one SIGINT delivery
 * @param sig Signal number (unused)
 */
static void count_sigint(int sig) {
    (void)sig;
    sigint_count++;
}

/* cppcheck-suppress-begin constParameter */
/**
 * @brief Count SIGINT deliveries and exit with that count
 * @param argc Argument count
 * @param argv Argument vector; argv[1] is an optional readiness file to create
 * @return Number of SIGINTs received (0 if none), 99 on setup failure
 */
int main(int argc, char *argv[]) {
    /* cppcheck-suppress-end constParameter */
    struct sigaction sa;
    int idx;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = count_sigint;
    if (sigemptyset(&sa.sa_mask) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
        return 99;
    }

    if (argc > 1) {
        int ready_fd = creat(argv[1], 0600);
        if (ready_fd >= 0) {
            if (close(ready_fd) != 0) {
                /* Nothing to report; the caller polls for the file. */
            }
        }
    }

    /* Wait for the first SIGINT. */
    for (idx = 0; idx < SIGCOUNT_WAIT_SLOTS && sigint_count == 0; idx++) {
        struct timespec slot = {0, 10000000L}; /* 10 ms */
        nanosleep(&slot, NULL);
    }

    /*
     * Linger afterwards. A duplicate delivered shortly after the first one
     * is only observable while this process is still running.
     */
    for (idx = 0; idx < SIGCOUNT_LINGER_SLOTS; idx++) {
        struct timespec slot = {0, 10000000L}; /* 10 ms */
        nanosleep(&slot, NULL);
    }

    return (int)sigint_count;
}
