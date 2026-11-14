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

/**
 * @brief Quit flag set by the termination-signal handler.
 */
static volatile sig_atomic_t quit_flag = 0;

/**
 * @brief Assert that sig_atomic_t can hold every handled signal number.
 */
#ifdef SIG_ATOMIC_MAX
#if SIG_ATOMIC_MAX < 127
#error "sig_atomic_t cannot hold the value 127"
#endif
#else
typedef char sig_atomic_large_enough[((sig_atomic_t)127 == 127) ? 1 : -1];
#endif

/**
 * @brief Number of the first termination signal received.
 */
static volatile sig_atomic_t quit_signal_num = 0;

/**
 * @brief Clear the internal quit flag and signal number.
 */
static void reset_signal_state(void) {
    quit_flag = 0;
    quit_signal_num = 0;
}

/**
 * @brief Record the first termination signal and set the quit flag.
 *
 * @param sig Signal number that triggered the handler.
 */
static void sig_handler(int sig) {
    if (quit_signal_num == 0) {
        quit_signal_num = (sig_atomic_t)sig;
    }
    quit_flag = 1;
}

/**
 * @brief Install one signal action for every termination signal.
 *
 * @param handler Handler to install, or SIG_DFL.
 * @return 0 on success, -1 on failure with errno set.
 */
static int set_signal_action(void (*handler)(int)) {
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
    /* SA_RESTART applies only to the custom handler, not to SIG_DFL. */
    act->sa_flags = (handler == SIG_DFL) ? 0 : SA_RESTART;

    ret = sigemptyset(&act->sa_mask);
    if (ret != 0) {
        goto error;
    }

    /* Block other termination signals so the first one owns the latch. */
    for (i = 0; i < num_sigs; i++) {
        /* Assign before comparing: Darwin's sigaddset macro folds to 0. */
        ret = sigaddset(&act->sa_mask, term_sigs[i]);
        if (ret != 0) {
            goto error;
        }
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

/**
 * @brief Install the unified termination-signal handler.
 *
 * Blocks all signals, clears the latch state, installs SA_RESTART handlers
 * for SIGINT, SIGQUIT, SIGTERM, SIGHUP and SIGPIPE, then restores the mask.
 *
 * @note Exits with an error when the mask or a registration fails.
 */
void configure_signal_handler(void) {
    sigset_t *block_mask = NULL, *old_mask = NULL;
    /* Every error path after the block must restore the original mask. */
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

    if (set_signal_action(sig_handler) != 0) {
        perror("Failed to set signal handler");
        goto error;
    }

    if (sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) {
        perror("sigprocmask restore");
        goto error;
    }

    free(block_mask);
    free(old_mask);
    return;

error:
    /* Undo the block first; the restore is idempotent. */
    if (blocked && sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) {
        perror("sigprocmask restore in error path");
    }
    free(block_mask);
    free(old_mask);
    exit(EXIT_FAILURE);
}

/**
 * @brief Check whether a termination signal has been received.
 *
 * @return 1 when a termination signal was caught, 0 otherwise.
 */
int is_quit_flag_set(void) {
    return !!quit_flag;
}

/**
 * @brief Get the signal number that set the quit flag.
 *
 * @return Number of the first termination signal, or 0 if none was received.
 */
int get_quit_signal(void) {
    return (int)quit_signal_num;
}

/**
 * @brief Reset the installed signal handlers to SIG_DFL.
 *
 * @return 0 on success, -1 on failure with errno set.
 */
int reset_signal_handlers_to_default(void) {
    if (set_signal_action(SIG_DFL) != 0) {
        perror("Failed to reset signal handlers");
        return -1;
    }
    return 0;
}
