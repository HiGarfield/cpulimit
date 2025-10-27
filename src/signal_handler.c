/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "signal_handler.h"
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/* Limiter quit flag */
static volatile sig_atomic_t limiter_quit_flag = 0;

/**
 * Signal handler for termination signals
 *
 * Handles termination signals such as SIGINT, SIGTERM, SIGHUP, and SIGQUIT.
 * When a signal is received, it sets the limiter_quit_flag to indicate
 * that the program should terminate gracefully.
 *
 * @param sig The signal number
 */
static void sig_handler(int sig)
{
    /* Handle the Ctrl+C or Ctrl+\ issue */
    ssize_t ret = write(STDOUT_FILENO, "\n", 1);

    limiter_quit_flag = 1;

    /* Suppress unused parameter or return value warnings */
    (void)sig;
    (void)ret;
}

void configure_signal_handlers(void)
{
    struct sigaction sa;
    size_t i;
    /* Signals that trigger application termination */
    static const int term_sigs[] = {
        SIGINT,  /* Terminal interrupt (Ctrl+C) */
        SIGTERM, /* Termination request (default kill) */
        SIGHUP,  /* Hangup on controlling terminal */
        SIGQUIT  /* Terminal quit (Ctrl+\) with core dump */
    };
    const size_t num_sigs = sizeof(term_sigs) / sizeof(*term_sigs);

    /* Initialize and configure sigaction structure */
    sigemptyset(&sa.sa_mask);    /* Block no extra signals in handler */
    sa.sa_handler = sig_handler; /* Use unified signal handler */
    sa.sa_flags = SA_RESTART;    /* Restart interrupted system calls */

    /* Register handler for each termination signal */
    for (i = 0; i < num_sigs; i++)
    {
        if (sigaction(term_sigs[i], &sa, NULL) != 0)
        {
            perror("Failed to set signal handler");
            exit(EXIT_FAILURE);
        }
    }
}

int is_quit_flag_set(void)
{
    return !!limiter_quit_flag;
}
