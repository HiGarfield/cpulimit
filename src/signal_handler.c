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
#include <stdio.h>
#include <stdlib.h>

volatile sig_atomic_t limiter_quit_flag = 0;

/**
 * Signal handler for SIGINT and SIGTERM signals.
 * Sets the limiter_quit_flag to 1 when a termination signal is received.
 *
 * @param sig Signal number (SIGINT or SIGTERM).
 */
static void sig_handler(int sig)
{
    (void)sig;
    limiter_quit_flag = 1;
}

void configure_signal_handlers(void)
{
    struct sigaction sa;
    size_t i;
    static const int terminate_signals[] = {SIGINT, SIGTERM};
    const size_t num_signals = sizeof(terminate_signals) / sizeof(*terminate_signals);

    /* Configure handler for termination signals */
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_RESTART;

    /* Set handlers for SIGINT and SIGTERM */
    for (i = 0; i < num_signals; i++)
    {
        if (sigaction(terminate_signals[i], &sa, NULL) != 0)
        {
            perror("Failed to set signal handler");
            exit(EXIT_FAILURE);
        }
    }
}
