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
 * @brief Grace period in milliseconds before SIGKILL escalation.
 */
#define CHILD_KILL_TIMEOUT_MS 5000

/**
 * @brief Reap the child without blocking on an error path.
 *
 * @param child_pid PID of the child to reap.
 *
 * @note The caller sends SIGCONT; a child that is still running is left to
 *       be reparented.
 */
void reap_child_before_error_return(pid_t child_pid) {
    for (;;) {
        int status;
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);
        if (wpid == child_pid) {
            return;
        }
        if (wpid == 0) {
            return;
        }
        if (wpid < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

/**
 * @def CHILD_POLL_INTERVAL_NS
 * @brief Nanoseconds between waitpid() polls during child cleanup.
 */
#define CHILD_POLL_INTERVAL_NS 50000000L

/**
 * @brief Wait for the child to exit and collect its exit status.
 *
 * @param child_pid PID of the child to wait for.
 * @param cfg Configuration, used for verbose output.
 * @param signal_forwarded Non-zero if the caller already forwarded the quit
 *        signal to the child's process group.
 * @return The child's exit status if reaped, EXIT_FAILURE otherwise.
 *
 * Polls with WNOHANG and maps death by signal to 128 + signal. After a quit
 * signal is forwarded, SIGKILL escalation is requested once, after
 * CHILD_KILL_TIMEOUT_MS. A clock failure resumes and reaps the child before
 * returning EXIT_FAILURE.
 */
int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              int signal_forwarded) {
    int child_exit_status = EXIT_FAILURE;
    int child_reaped = 0;
    int kill_sent = 0;
    struct timespec start_time;

    if (get_current_time(&start_time) != 0) {
        perror("get_current_time");
        /* Resume and reap the child before leaving an error path. */
        kill(child_pid, SIGCONT);
        reap_child_before_error_return(child_pid);
        return EXIT_FAILURE;
    }

    /* waitpid() reaps only direct children. */
    while (1) {
        int status;
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
                kill(child_pid, SIGCONT);
                reap_child_before_error_return(child_pid);
                return EXIT_FAILURE;
            }

            /* Forward a late quit signal once, after SIGCONT. */
            if (is_quit_flag_set() && !signal_forwarded) {
                signal_command(child_pid, SIGCONT, cfg->verbose);
                forward_quit_signal(child_pid, cfg->verbose);
                signal_forwarded = 1;
                if (get_current_time(&start_time) != 0) {
                    perror("get_current_time");
                    kill(child_pid, SIGCONT);
                    reap_child_before_error_return(child_pid);
                    return EXIT_FAILURE;
                }
            } else if (signal_forwarded) {
                /* Escalate after the grace period to reach descendants. */
                double elapsed_ms;
                elapsed_ms = timediff_in_ms(&current_time, &start_time);
                if (elapsed_ms > (double)CHILD_KILL_TIMEOUT_MS && !kill_sent) {
                    /* Escalate only once to avoid repeated group signals. */
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
