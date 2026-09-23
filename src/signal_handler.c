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
#include <unistd.h>

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
 * other termination signals like SIGTERM, SIGHUP, or SIGPIPE.
 */
static volatile sig_atomic_t tty_quit_flag = 0;

/**
 * @brief Compile-time assertion: sig_atomic_t can hold values up to 127
 *
 * POSIX.1-2001 requires SIG_ATOMIC_MAX >= 127, so any value in the range
 * [0, 127] can be stored in a sig_atomic_t without truncation or overflow.
 * In this module we only store a small set of termination signal numbers
 * (SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM) in quit_signal_num, all of
 * which are well within that guaranteed range.
 * When SIG_ATOMIC_MAX is available as a preprocessor constant, a direct
 * #if check is used; otherwise a C89-compatible typedef (negative array
 * size trick) provides the equivalent guarantee.
 */
#ifdef SIG_ATOMIC_MAX
#if SIG_ATOMIC_MAX < 127
#error "sig_atomic_t cannot hold the value 127"
#endif
#else
typedef char sig_atomic_large_enough[((sig_atomic_t)127 == 127) ? 1 : -1];
#endif

/**
 * @brief Signal number of the first termination signal received
 *
 * Records the signal number that caused the quit flag to be set.
 * Initialized to 0 (no signal). Set once when the first termination
 * signal is received; subsequent signals do not overwrite it.
 * All stored signal numbers for the handled signals (SIGHUP, SIGINT,
 * SIGQUIT, SIGPIPE, SIGTERM) are expected to fall within the
 * POSIX.1-2001 guaranteed sig_atomic_t range [0, 127].
 */
static volatile sig_atomic_t quit_signal_num = 0;

/**
 * @brief Non-zero once the newline that ends the keyboard-quit echo was
 *        written
 *
 * One newline per run is enough, and a run can end by more than one route --
 * the limiting loop knows it is over, and so does main() -- so the callers ask
 * without having to find out whether the other already did.  Cleared with the
 * other per-run flags in reset_signal_state().  A plain int, not a
 * sig_atomic_t: unlike the flags above, this one is only ever touched in
 * process context.
 */
static int tty_newline_written = 0;

/**
 * @brief Reset internal signal-handler state flags to their initial values
 *
 * Clears quit_flag, tty_quit_flag, quit_signal_num and the newline marker so
 * subsequent monitoring sessions start from a clean state. Intended to be
 * called during signal-handler setup in process context (never from within a
 * signal handler).
 */
static void reset_signal_state(void) {
    quit_flag = 0;
    tty_quit_flag = 0;
    quit_signal_num = 0;
    tty_newline_written = 0;
}

/**
 * @brief Unified signal handler for termination signals
 * @param sig Signal number that triggered this handler
 *
 * Handles SIGINT, SIGQUIT, SIGTERM, SIGHUP, and SIGPIPE by setting the quit
 * flag. For terminal-originated signals (SIGINT from Ctrl+C, SIGQUIT from
 * Ctrl+\), also sets the TTY termination flag to distinguish these from other
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
 * @brief Internal common routine to install a specific signal action
 * @param handler Handler to install (sig_handler or SIG_DFL)
 * @return 0 on success, -1 on failure (errno is set by underlying syscalls)
 *
 * Uses a fixed internal list of termination signals. All error handling is
 * delegated to the caller.
 */
static int set_signal_action(void (*handler)(int)) {
    /* Fixed list of termination signals handled by this module */
    static const int term_sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE};
    static const size_t num_sigs = sizeof(term_sigs) / sizeof(*term_sigs);

    struct sigaction *act = NULL;
    size_t i;
    int ret;

    act = (struct sigaction *)calloc(1, sizeof(*act));
    if (act == NULL) {
        goto error;
    }

    act->sa_handler = handler;
    /* Only enable SA_RESTART for our custom handler, not for SIG_DFL */
    act->sa_flags = (handler == SIG_DFL) ? 0 : SA_RESTART;

    ret = sigemptyset(&act->sa_mask);
    if (ret != 0) {
        goto error;
    }

    for (i = 0; i < num_sigs; i++) {
        if (sigaction(term_sigs[i], act, NULL) != 0) {
            goto error;
        }
    }

    free(act);
    return 0;

error:
    free(act);
    return -1;
}

void configure_signal_handler(void) {
    /* Initialize to NULL to make free(NULL) safe in error paths */
    sigset_t *block_mask = NULL, *old_mask = NULL;
    /*
     * Non-zero once all signals are blocked: every error path after that
     * point must restore the original mask before leaving, because the
     * documented contract is that the mask is unchanged on return.
     */
    int blocked = 0;
    int ret;

    block_mask = (sigset_t *)calloc(1, sizeof(*block_mask));
    if (block_mask == NULL) {
        fprintf(stderr, "Memory allocation failed for block_mask\n");
        goto error;
    }

    old_mask = (sigset_t *)calloc(1, sizeof(*old_mask));
    if (old_mask == NULL) {
        fprintf(stderr, "Memory allocation failed for old_mask\n");
        goto error;
    }

    /* Block all signals at function entry to eliminate race conditions */
    ret = sigfillset(block_mask);
    if (ret != 0) {
        perror("sigfillset");
        goto error;
    }

    if (sigprocmask(SIG_BLOCK, block_mask, old_mask) != 0) {
        perror("sigprocmask");
        goto error;
    }
    blocked = 1;

    reset_signal_state();

    /* Install handlers; jump to cleanup on failure */
    if (set_signal_action(sig_handler) != 0) {
        perror("Failed to set signal handler");
        goto error;
    }

    /* Restore the original signal mask */
    if (sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) {
        perror("sigprocmask restore");
        goto error;
    }

    /* Normal execution path: clean up resources and return */
    free(block_mask);
    free(old_mask);
    return;

error:
    /* Centralized error handling */
    if (blocked) {
        /*
         * Undo the all-signals block first.  Idempotent: the normal path
         * jumping here after a failed restore simply restores once more.
         */
        if (sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) {
            perror("sigprocmask restore in error path");
        }
    }
    free(block_mask);
    free(old_mask);
    exit(EXIT_FAILURE);
}

int is_quit_flag_set(void) {
    return !!quit_flag;
}

int is_terminated_by_tty(void) {
    return !!tty_quit_flag;
}

void finish_tty_quit_line(void) {
    if (tty_newline_written || !quit_flag || !tty_quit_flag) {
        return;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return;
    }
    tty_newline_written = 1;
    fputc('\n', stdout);
    fflush(stdout);
}

int get_quit_signal(void) {
    return (int)quit_signal_num;
}

int reset_signal_handlers_to_default(void) {
    if (set_signal_action(SIG_DFL) != 0) {
        perror("Failed to reset signal handlers");
        return -1;
    }
    return 0;
}
