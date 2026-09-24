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

#include "cli.h"
#include "signal_forward.h"
#include "signal_handler.h"
#include "time_util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

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
 * @brief Reap the child before returning from an internal failure
 * @param child_pid PID of the child to collect
 *
 * An error return has to leave no zombie behind, but this never blocks: every
 * caller returns EXIT_FAILURE right after, and main() exits, so the child is
 * reparented to init and reaped there rather than staying a zombie of this
 * process.  Blocking would turn a nearly unreachable error branch into a hang
 * -- "cpulimit -l 50 -- sh -c 'trap "" TERM; sleep 100000'" would wait for the
 * sleep to end even though the forwarded SIGTERM can simply be ignored, and
 * this path bypasses the polling loop's SIGKILL escalation that would
 * otherwise bound the wait.  So the wait is always WNOHANG: if the child
 * has not exited yet this returns immediately.  EINTR is retried because
 * waitpid() is interruptible; any other error means there is nothing left to
 * collect.
 *
 * The caller sends SIGCONT before calling this so a child that was stopped (by
 * this program's own throttle or an earlier forwarded signal) is resumed; this
 * function only handles reaping, and leaving a child stopped would be the real
 * defect to avoid.
 *
 * Used by the internal failure paths, which return EXIT_FAILURE to the caller
 * instead of terminating the process underneath it.
 */
static void reap_child_before_error_return(pid_t child_pid) {
    for (;;) {
        int status;
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);
        if (wpid == child_pid) {
            return;
        }
        if (wpid == 0) {
            /*
             * The child is still running; do not block waiting for it.
             * The caller is returning anyway and the child is reparented
             * to init when this process exits.
             */
            return;
        }
        if (wpid < 0 && errno == EINTR) {
            continue;
        }
        /* ECHILD or a real error: there is nothing left to collect. */
        return;
    }
}

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
                              int signal_forwarded) {
    /* Default exit status if child is not properly reaped */
    int child_exit_status = EXIT_FAILURE;
    /* 1 if child PID was successfully reaped, 0 otherwise */
    int child_reaped = 0;
    /* 1 once the SIGKILL escalation has been sent, so it happens once */
    int kill_sent = 0;
    /* Timeout anchor; reset when forwarding signal */
    struct timespec start_time;

    /* Record time for timeout monitoring during cleanup */
    if (get_current_time(&start_time) != 0) {
        perror("get_current_time");
        /*
         * Return instead of exiting: the caller still has its own
         * diagnosis and exit status to produce, and terminating the
         * process here skipped both.  The child is resumed first so
         * it does not stay stopped, then reaped.  Reaping is always
         * non-blocking (WNOHANG): even if the child ignores the signal and
         * keeps running, this returns immediately and the child is
         * reparented to init on exit.
         */
        kill(child_pid, SIGCONT);
        reap_child_before_error_return(child_pid);
        return EXIT_FAILURE;
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
                /* Same reasoning as above: reap, then let the caller
                 * decide how the run ends.  Reaping is non-blocking
                 * (WNOHANG) regardless; the child is reparented to init if
                 * it keeps running. */
                kill(child_pid, SIGCONT);
                reap_child_before_error_return(child_pid);
                return EXIT_FAILURE;
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
                    /* Same reasoning as above: reap, then let the caller
                       decide how the run ends.  Reaping is non-blocking
                       (WNOHANG) regardless; the child is reparented to init
                       if it keeps running. */
                    kill(child_pid, SIGCONT);
                    reap_child_before_error_return(child_pid);
                    return EXIT_FAILURE;
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
                if (elapsed_ms > (double)CHILD_KILL_TIMEOUT_MS && !kill_sent) {
                    /*
                     * Escalate exactly once.  Every poll past the
                     * threshold used to send another SIGKILL to the
                     * child's process group: for the whole window in
                     * which the child has exited but is not reaped yet a
                     * zombie still carries its PID and PGID, so the
                     * repeats reached whatever else was living in that
                     * group, and signal_command() printed its own
                     * failure message again each time.
                     */
                    if (cfg->verbose) {
                        printf("Process %ld timed out, sending SIGKILL\n",
                               (long)child_pid);
                    }
                    /* SIGKILL cannot be caught or ignored */
                    signal_command(child_pid, SIGKILL);
                    kill_sent = 1;
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

#ifdef CPULIMIT_TEST_BUILD
/*
 * Test-only accessor for reap_child_before_error_return(): a child that may
 * ignore the termination signal is resumed (SIGCONT) and reaped.  The call must
 * return at once (WNOHANG); it must never block waiting for the child.  Defined
 * only in the test build so the production object stays free of test code.
 */
int cpulimit_test_exercise_reap(pid_t child_pid);

int cpulimit_test_exercise_reap(pid_t child_pid) {
    kill(child_pid, SIGCONT);
    reap_child_before_error_return(child_pid);
    return 0;
}
#endif
