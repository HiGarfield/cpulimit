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

#include "child_wait.h"

#include "signal_forward.h"
#include "signal_handler.h"
#include "time_util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

/**
 * @def CHILD_KILL_TIMEOUT_MS
 * @brief Timeout in milliseconds before escalating to SIGKILL
 *
 * After the limiter decides to terminate the child process group, it
 * polls waitpid() in a loop.  If the child has not exited within this
 * many milliseconds, SIGKILL is sent to the entire process group.
 */
#define CHILD_KILL_TIMEOUT_MS 5000

/**
 * @def CHILD_POLL_INTERVAL_NS
 * @brief Nanoseconds between waitpid() polls during child cleanup
 *
 * While waiting for the child to exit (in collect_child_exit_status()),
 * the parent sleeps for this interval between non-blocking waitpid()
 * calls to avoid busy-waiting.
 */
#define CHILD_POLL_INTERVAL_NS 50000000L /* 50 ms */

int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              volatile int signal_forwarded) {
    /* Default exit status if child is not properly reaped */
    int child_exit_status = EXIT_FAILURE;
    /* 1 if child PID was successfully reaped, 0 otherwise */
    int child_reaped = 0;
    /* Timeout anchor; reset when forwarding signal */
    struct timespec start_time;

    /* Record time for timeout monitoring during cleanup */
    if (get_current_time(&start_time) != 0) {
        perror("get_current_time");
        kill(child_pid, SIGCONT);
        exit(EXIT_FAILURE);
    }

    /*
     * Cleanup loop: wait for the command child process to exit.
     * waitpid() can only reap direct children of this process.
     * Any grandchildren (forked by child_pid) are not direct
     * children of this process; they are reparented to init when
     * child_pid exits, matching standard shell semantics.
     * Use a positive PID — the negative-PGID form waitpid(-pgid)
     * would also only return child_pid since it is our only
     * direct child, but using the positive form is clearer and
     * eliminates a dead code branch.
     */
    while (1) {
        int status;
        /*
         * Poll for child state change without blocking (WNOHANG).
         * Returns 0 if no state change has occurred yet, child's
         * PID if it has changed state, or -1 on error.
         */
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);

        if (wpid == child_pid) {
            /* Child process has terminated; record exit status */
            child_reaped = 1;

            if (WIFEXITED(status)) {
                /* Child exited normally via exit() or return from main() */
                child_exit_status = WEXITSTATUS(status);
                if (cfg->verbose) {
                    printf("Process %ld exited with status %d\n",
                           (long)child_pid, child_exit_status);
                }
            } else if (WIFSIGNALED(status)) {
                /* Child was terminated by a signal (SIGTERM, SIGKILL, etc) */
                int signal_number = WTERMSIG(status);
                /*
                 * Shell convention: exit status = 128 + signal number
                 * Example: SIGTERM (15) -> exit status 143
                 */
                child_exit_status = 128 + signal_number;
                if (cfg->verbose) {
                    printf("Process %ld terminated by signal %d\n",
                           (long)child_pid, signal_number);
                }
            } else {
                /* Abnormal termination (neither exit nor signal) */
                if (cfg->verbose) {
                    printf("Process %ld terminated abnormally\n",
                           (long)child_pid);
                }
                child_exit_status = EXIT_FAILURE;
            }
            break;
        }
        if (wpid == 0) {
            /*
             * No state changes yet (WNOHANG returned immediately).
             * Check if we've exceeded timeout for graceful termination.
             */
            const struct timespec poll_sleep = {0, CHILD_POLL_INTERVAL_NS};
            struct timespec current_time;
            if (get_current_time(&current_time) != 0) {
                perror("get_current_time");
                kill(child_pid, SIGCONT);
                exit(EXIT_FAILURE);
            }

            /*
             * Forward the quit signal exactly once.
             *
             * run_command_mode() normally forwards it before calling this
             * function, so signal_forwarded is already non-zero and there
             * is nothing to do: the child is already shutting down and a
             * second delivery would only interrupt it.
             *
             * The quit flag can also be set afterwards (the late-arrival
             * race on macOS 10.7, where limit_process() returns before the
             * signal arrives).  Then the child has not been told to stop,
             * so the signal is forwarded here on the first poll that
             * observes it.  SIGCONT is sent first so a stopped child is
             * resumed before the forwarded signal is delivered.
             */
            if (is_quit_flag_set() && !signal_forwarded) {
                signal_command(child_pid, SIGCONT);
                forward_quit_signal(child_pid);
                signal_forwarded = 1;
                /*
                 * Reset the timeout anchor to the moment the signal is
                 * forwarded so the child gets the full
                 * CHILD_KILL_TIMEOUT_MS grace period from this point,
                 * not from function entry.
                 */
                if (get_current_time(&start_time) != 0) {
                    perror("get_current_time");
                    kill(child_pid, SIGCONT);
                    exit(EXIT_FAILURE);
                }
            } else if (signal_forwarded) {
                /*
                 * After CHILD_KILL_TIMEOUT_MS, forcefully kill any
                 * remaining processes.  This handles cases where
                 * processes ignore SIGTERM.  The process group is
                 * targeted so that descendants that are still running
                 * are terminated too, even though they cannot be
                 * wait()ed on directly.
                 *
                 * The escalation enforces a termination request, so it
                 * is armed only once one has been forwarded.
                 * limit_process() also returns when the target is no
                 * longer visible to the process iterator without having
                 * exited, and a command that is still running must not
                 * be killed for that: cpulimit then simply waits for
                 * it, the way a shell does.
                 */
                double elapsed_ms;
                elapsed_ms = timediff_in_ms(&current_time, &start_time);
                if (elapsed_ms > (double)CHILD_KILL_TIMEOUT_MS) {
                    if (cfg->verbose) {
                        printf("Process %ld timed out, sending SIGKILL\n",
                               (long)child_pid);
                    }
                    /* SIGKILL cannot be caught or ignored */
                    signal_command(child_pid, SIGKILL);
                }
            }
            /* Brief sleep to avoid busy-waiting */
            sleep_timespec(&poll_sleep);

        } else {
            /* wpid < 0: waitpid() encountered an error */
            if (errno == EINTR) {
                /* Interrupted by signal, retry immediately */
                continue;
            }
            if (errno != ECHILD) {
                /* Real error (not just "no children") */
                perror("waitpid");
            }
            /* ECHILD means child already reaped or no children */
            break;
        }
    }

    /*
     * Return child's exit status if we successfully reaped it,
     * otherwise return failure status.
     */
    return child_reaped ? child_exit_status : EXIT_FAILURE;
}
