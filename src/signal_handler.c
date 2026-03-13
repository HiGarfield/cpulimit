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
 * @brief Compile-time assertion: sig_atomic_t can hold values up to 127.
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
 * @brief Reset internal signal-handler state flags to their initial values
 *
 * Clears quit_flag, tty_quit_flag, and quit_signal_num so subsequent
 * monitoring sessions start from a clean state. Intended to be called during
 * signal-handler setup in process context (never from within a signal
 * handler).
 */
static void reset_signal_state(void) {
    quit_flag = 0;
    tty_quit_flag = 0;
    quit_signal_num = 0;
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
 * @brief Set up signal handlers for graceful program termination
 *
 * Registers a unified signal handler for SIGINT (Ctrl+C), SIGQUIT (Ctrl+\),
 * SIGTERM, SIGHUP, and SIGPIPE signals. When any of these signals are
 * received, the handler sets a quit flag that can be checked via
 * is_quit_flag_set(). For terminal-originated signals (SIGINT, SIGQUIT),
 * also sets a flag indicating TTY termination. The handler uses SA_RESTART
 * to automatically restart interrupted system calls.
 *
 * All signals are blocked at function entry (sigfillset + sigprocmask are
 * the very first operations) so that no termination signal can be delivered
 * before reset_signal_state() or the sigaction loop. The internal
 * signal-latch state (quit_flag, tty_quit_flag, quit_signal_num) is then
 * cleared, new handlers are installed, and the original mask is restored.
 * Any signal that becomes pending during the blocked window is delivered
 * through the new handlers once the mask is restored.
 *
 * @note Exits with error if signal mask or handler registration fails
 */
void configure_signal_handler(void) {
    struct sigaction sig_action;
    sigset_t block_mask, old_mask;
    size_t sig_idx;
    /* Array of signals that should trigger graceful termination */
    static const int term_sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE};
    static const size_t num_sigs = sizeof(term_sigs) / sizeof(*term_sigs);

    /* Block all signals at function entry so that no termination signal can
     * be delivered between here and the completion of handler installation.
     * This eliminates the race where a signal fires before sigprocmask takes
     * effect, sets quit_flag, and then has that state wiped by the subsequent
     * reset_signal_state() call. SIGKILL and SIGSTOP cannot be blocked and
     * are silently ignored by sigprocmask, which is harmless. The original
     * mask is restored after all handlers are in place.
     * sigfillset() always returns 0 per POSIX.1-2001 and is not checked. */
    (void)sigfillset(&block_mask);
    if (sigprocmask(SIG_BLOCK, &block_mask, &old_mask) != 0) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    /* Configure sigaction structure with unified handler */
    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_handler =
        sig_handler; /* Unified handler for all termination signals */
    sig_action.sa_flags =
        SA_RESTART; /* Automatically restart interrupted syscalls */
    if (sigemptyset(&sig_action.sa_mask) != 0) {
        perror("sigemptyset");
        exit(EXIT_FAILURE);
    }

    /* Start from a deterministic state for each new configuration. */
    reset_signal_state();

    /* Register the same handler for all termination signals */
    for (sig_idx = 0; sig_idx < num_sigs; sig_idx++) {
        if (sigaction(term_sigs[sig_idx], &sig_action, NULL) != 0) {
            perror("Failed to set signal handler");
            exit(EXIT_FAILURE);
        }
    }

    /* Restore the original signal mask; any signals pending during the
     * blocked window are now delivered through the newly installed handlers. */
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
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

/**
 * @brief Reset all signal handlers installed by configure_signal_handler()
 *        back to their default dispositions (SIG_DFL)
 * @return 0 on success, -1 on failure (errno set; error logged to stderr)
 *
 * Resets SIGINT, SIGQUIT, SIGTERM, SIGHUP, and SIGPIPE to SIG_DFL.
 */
int reset_signal_handlers_to_default(void) {
    struct sigaction def_action;
    /* Signals installed by configure_signal_handler() */
    static const int reset_sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE};
    static const size_t num_sigs = sizeof(reset_sigs) / sizeof(*reset_sigs);
    size_t sig_idx;

    memset(&def_action, 0, sizeof(def_action));
    def_action.sa_handler = SIG_DFL;
    if (sigemptyset(&def_action.sa_mask) != 0) {
        perror("sigemptyset");
        return -1;
    }
    for (sig_idx = 0; sig_idx < num_sigs; sig_idx++) {
        if (sigaction(reset_sigs[sig_idx], &def_action, NULL) != 0) {
            perror("sigaction reset");
            return -1;
        }
    }
    return 0;
}
