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
 * polls waitpid() in a loop. If the child has not exited within this
 * many milliseconds, SIGKILL is sent to the entire process group.
 */
#define CHILD_KILL_TIMEOUT_MS 5000

/**
 * @brief Resume and reap the command's child, without blocking on it
 *
 * @param child_pid PID of the child process to reap
 *
 * For failure paths that return EXIT_FAILURE: the child is still out there, so
 * resume it (the caller already sent SIGCONT) and reap it with WNOHANG -- a
 * blocking wait would hang on a child that ignores the signal, since this path
 * skips the polling loop's SIGKILL escalation. A child that has not exited is
 * reparented to init.
 */
void reap_child_before_error_return(pid_t child_pid) {
    for (;;) {
        int status;
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);
        if (wpid == child_pid) {
            return;
        }
        if (wpid == 0) {
            /* Child still running; caller returns and it is reparented to init.
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

/**
 * @brief Wait for the child to exit and collect its exit status
 *
 * @param child_pid PID of the child process to wait for
 * @param cfg Pointer to configuration structure (used for verbose output)
 * @param signal_forwarded Non-zero if the caller already forwarded the quit
 *                         signal to the child process group
 * @return The child's exit status if reaped, EXIT_FAILURE otherwise
 *
 * Polls with WNOHANG, translating death by signal into 128 + signal. Once the
 * quit signal is forwarded, escalates to SIGKILL after CHILD_KILL_TIMEOUT_MS
 * (once, on the group); a late quit signal is forwarded here at most once. An
 * unreadable clock does not end the process: the child is resumed and reaped
 * first, then EXIT_FAILURE is returned.
 */
int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              int signal_forwarded) {
    int child_exit_status = EXIT_FAILURE;
    int child_reaped = 0;
    /* 1 once the SIGKILL escalation has been sent, so it happens once */
    int kill_sent = 0;
    /* Timeout anchor; reset when forwarding signal */
    struct timespec start_time;

    if (get_current_time(&start_time) != 0) {
        perror("get_current_time");
        /* Return so the caller can finish its own diagnosis; resume and
         * reap the child first so it is not left stopped. */
        kill(child_pid, SIGCONT);
        reap_child_before_error_return(child_pid);
        return EXIT_FAILURE;
    }

    /* waitpid() reaps only direct children; grandchildren are
     * reparented to init when child_pid exits. */
    while (1) {
        int status;
        /* Non-blocking poll; 0 = no change, PID = changed, -1 = error. */
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);

        if (wpid == child_pid) {
            child_reaped = 1;

            if (WIFEXITED(status)) {
                child_exit_status = WEXITSTATUS(status);
                if (cfg->verbose) {
                    printf("Process %ld exited with status %d\n",
                           (long)child_pid, child_exit_status);
                }
            } else if (WIFSIGNALED(status)) {
                int signal_number = WTERMSIG(status);
                /* Shell convention: exit status = 128 + signal number. */
                child_exit_status = 128 + signal_number;
                if (cfg->verbose) {
                    printf("Process %ld terminated by signal %d\n",
                           (long)child_pid, signal_number);
                }
            } else {
                if (cfg->verbose) {
                    printf("Process %ld terminated abnormally\n",
                           (long)child_pid);
                }
                child_exit_status = EXIT_FAILURE;
            }
            break;
        }
        if (wpid == 0) {
            const struct timespec poll_sleep = {0, CHILD_POLL_INTERVAL_NS};
            struct timespec current_time;
            if (get_current_time(&current_time) != 0) {
                perror("get_current_time");
                /* Same as the error path above: resume and reap the child. */
                kill(child_pid, SIGCONT);
                reap_child_before_error_return(child_pid);
                return EXIT_FAILURE;
            }

            /*
             * Forward the quit signal at most once. run_command_mode()
             * usually forwards it first (signal_forwarded already set), so
             * normally nothing to do. If the quit flag is set later, such
             * as the macOS 10.7 late-signal race, forward it here, sending
             * SIGCONT first so a stopped child resumes beforehand.
             */
            if (is_quit_flag_set() && !signal_forwarded) {
                signal_command(child_pid, SIGCONT, cfg->verbose);
                forward_quit_signal(child_pid, cfg->verbose);
                signal_forwarded = 1;
                /* Reset the timeout anchor now so the full grace period
                 * applies. */
                if (get_current_time(&start_time) != 0) {
                    perror("get_current_time");
                    /* Same as the error path above: resume and reap the child.
                     */
                    kill(child_pid, SIGCONT);
                    reap_child_before_error_return(child_pid);
                    return EXIT_FAILURE;
                }
            } else if (signal_forwarded) {
                /*
                 * Past CHILD_KILL_TIMEOUT_MS, escalate to SIGKILL on the
                 * process group so still-running descendants die too.
                 * Armed only after a quit signal was forwarded; if the
                 * target just became invisible (not exited), cpulimit
                 * waits like a shell rather than killing it.
                 */
                double elapsed_ms;
                elapsed_ms = timediff_in_ms(&current_time, &start_time);
                if (elapsed_ms > (double)CHILD_KILL_TIMEOUT_MS && !kill_sent) {
                    /* Escalate to SIGKILL exactly once; repeats would hit
                     * other processes sharing the child's group. */
                    if (cfg->verbose) {
                        printf("Process %ld timed out, sending SIGKILL\n",
                               (long)child_pid);
                    }
                    signal_command(child_pid, SIGKILL, cfg->verbose);
                    kill_sent = 1;
                }
            }
            sleep_timespec(&poll_sleep);

        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                perror("waitpid");
            }
            break;
        }
    }

    return child_reaped ? child_exit_status : EXIT_FAILURE;
}
