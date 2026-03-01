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

#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Global quit flag indicating a termination signal was received
 *
 * Type sig_atomic_t ensures atomic access in signal handlers.
 * Volatile qualifier prevents compiler optimizations that could cache the
 * value.
 */
static volatile sig_atomic_t quit_flag = 0;

/**
 * @brief Flag indicating termination originated from terminal keyboard input
 *
 * Set to 1 for SIGINT (Ctrl+C) and SIGQUIT (Ctrl+\), remains 0 for
 * other termination signals like SIGTERM or SIGHUP.
 */
static volatile sig_atomic_t tty_quit_flag = 0;

/**
 * @brief Signal number of the first termination signal received
 *
 * Records the signal number that caused the quit flag to be set.
 * Initialized to 0 (no signal). Set once when the first termination
 * signal is received; subsequent signals do not overwrite it.
 */
static volatile sig_atomic_t quit_signal_num = 0;

/**
 * @brief Unified signal handler for termination signals
 * @param sig Signal number that triggered this handler
 *
 * Handles SIGINT, SIGQUIT, SIGTERM, and SIGHUP by setting the quit flag.
 * For terminal-originated signals (SIGINT from Ctrl+C, SIGQUIT from Ctrl+\),
 * also sets the TTY termination flag to distinguish these from other
 * termination requests. Records the first received signal number for later
 * forwarding. Uses only async-signal-safe operations.
 */
static void sig_handler(int sig) {
    /* Mark TTY-originated signals for special handling */
    switch (sig) {
    case SIGINT:  /* Ctrl+C */
    case SIGQUIT: /* Ctrl+\ */
        tty_quit_flag = 1;
        break;
    default:
        break;
    }
    /* Record signal number of first received termination signal */
    if (quit_signal_num == 0) {
        quit_signal_num = (sig_atomic_t)sig;
    }
    /* Set global quit flag to initiate graceful shutdown */
    quit_flag = 1;
}

/**
 * @brief Set up signal handlers for graceful program termination
 *
 * Registers a unified signal handler for SIGINT (Ctrl+C), SIGQUIT (Ctrl+\),
 * SIGTERM, SIGHUP, and SIGPIPE signals. When any of these signals are
 * received, the handler sets a quit flag that can be checked via
 * is_quit_flag_set(). For terminal-originated signals (SIGINT, SIGQUIT),
 * also sets a flag indicating TTY termination. The handler uses SA_RESTART
 * to automatically restart interrupted system calls.
 *
 * @note Exits with error if signal registration fails
 */
void configure_signal_handler(void) {
    struct sigaction sig_action;
    size_t sig_idx;
    /* Array of signals that should trigger graceful termination */
    static const int term_sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE};
    static const size_t num_sigs = sizeof(term_sigs) / sizeof(*term_sigs);

    /* Configure sigaction structure with unified handler */
    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_handler =
        sig_handler; /* Unified handler for all termination signals */
    sig_action.sa_flags =
        SA_RESTART; /* Automatically restart interrupted syscalls */
    sigemptyset(
        &sig_action
             .sa_mask); /* Don't block additional signals during handler */

    /* Register the same handler for all termination signals */
    for (sig_idx = 0; sig_idx < num_sigs; sig_idx++) {
        if (sigaction(term_sigs[sig_idx], &sig_action, NULL) != 0) {
            perror("Failed to set signal handler");
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * @brief Check if a termination signal has been received
 * @return 1 if a termination signal was caught, 0 otherwise
 *
 * Returns the state of the quit flag, which is set by the signal handler
 * when SIGINT, SIGQUIT, SIGTERM, or SIGHUP is received. The main program
 * loop should periodically check this flag to initiate graceful shutdown.
 */
int is_quit_flag_set(void) {
    return !!quit_flag;
}

/**
 * @brief Check if termination was triggered by terminal keyboard input
 * @return 1 if terminated by SIGINT or SIGQUIT, 0 otherwise
 *
 * Distinguishes between terminal-originated termination (Ctrl+C or Ctrl+\)
 * and other termination signals (SIGTERM, SIGHUP). This can be used to
 * customize shutdown behavior or messages based on how termination occurred.
 */
int is_terminated_by_tty(void) {
    return !!tty_quit_flag;
}

/**
 * @brief Get the signal number that caused the quit flag to be set
 * @return Signal number (e.g. SIGTERM, SIGINT) of the first received
 *         termination signal, or 0 if no termination signal has been
 *         received yet
 *
 * Returns the signal number recorded when the first termination signal
 * was delivered to the process. This allows callers to forward the exact
 * received signal to child processes, ensuring consistent behavior with
 * a standard shell (e.g., Ctrl+C sends SIGINT to the child, not SIGTERM).
 */
int get_quit_signal(void) {
    return (int)quit_signal_num;
}
