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

#include "signal_handler.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Limiter quit flag indicating program termination request */
static volatile sig_atomic_t limiter_quit_flag = 0;

/* Flag indicating program terminated by terminal key input */
static volatile sig_atomic_t terminated_by_tty = 0;

/**
 * @brief Signal handler for termination signals
 * @param sig The signal number
 * @note This handler sets the quit flag to indicate the program should
 *       terminate gracefully. It handles SIGINT, SIGTERM, SIGHUP, and
 *       SIGQUIT signals.
 */
static void sig_handler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGQUIT:
        terminated_by_tty = 1;
        break;
    default:
        break;
    }
    limiter_quit_flag = 1;
}

/**
 * @brief Configure signal handler for graceful termination
 * @note This function sets up signal handler for termination signals
 *       to allow the program to exit gracefully.
 */
void configure_signal_handler(void) {
    struct sigaction sa;
    size_t i;
    /* Signals that trigger application termination */
    static const int term_sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP};
    static const size_t num_sigs = sizeof(term_sigs) / sizeof(*term_sigs);

    /* Initialize and configure sigaction structure */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler; /* Use unified signal handler */
    sa.sa_flags = SA_RESTART;    /* Restart interrupted system calls */
    sigemptyset(&sa.sa_mask);    /* No extra signals blocked */

    /* Register handler for each termination signal */
    for (i = 0; i < num_sigs; i++) {
        if (sigaction(term_sigs[i], &sa, NULL) != 0) {
            perror("Failed to set signal handler");
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * @brief Check if the quit flag is set
 * @return 1 if the quit flag is set, 0 otherwise
 */
int is_quit_flag_set(void) {
    return !!limiter_quit_flag;
}

/**
 * @brief Check if the program was terminated via terminal input
 * @return 1 if  termination was caused by terminal key (Ctrl+C, Ctrl+\),
 *         0 otherwise
 */
int is_terminated_by_tty(void) {
    return !!terminated_by_tty;
}
