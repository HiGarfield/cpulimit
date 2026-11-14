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

#include "limiter.h"

#include "child_exec.h"
#include "child_wait.h"
#include "cli.h"
#include "exec_sync.h"
#include "limit_process.h"
#include "process_finder.h"
#include "process_iterator.h"
#include "process_set.h"
#include "signal_forward.h"
#include "signal_handler.h"
#include "time_util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Execute and monitor a command with CPU limiting.
 *
 * @param cfg Configuration with the command and options.
 * @return The process exit status.
 *
 * Forks the child into its own process group, applies the limit and waits
 * for completion, escalating to SIGKILL after a termination timeout.
 */
int run_command_mode(const struct cpulimit_cfg *cfg) {
    pid_t child_pid;
    int sync_pipe[2];
    int fd_flags;
    int forwarded_quit_signal;
    int limit_status = LIMIT_PROCESS_OK;

    /* The parent's read waits for FD_CLOEXEC to close the write end. */
    if (pipe(sync_pipe) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    fd_flags = fcntl(sync_pipe[1], F_GETFD);
    /* Unsigned operands avoid a signed bitwise OR. */
    if (fd_flags < 0 ||
        fcntl(sync_pipe[1], F_SETFD,
              (int)((unsigned int)fd_flags | (unsigned int)FD_CLOEXEC)) < 0) {
        perror("fcntl");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return EXIT_FAILURE;
    }

    /* Flush before fork so buffered output is not duplicated. */
    fflush(stdout);
    fflush(stderr);

    child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        exec_child_process(cfg, sync_pipe[0], sync_pipe[1]);
        /* exec_child_process() never returns. */
    }

    close(sync_pipe[1]);
    if (wait_for_child_exec(child_pid, sync_pipe[0]) != 0) {
        return EXIT_FAILURE;
    }

    if (cfg->verbose) {
        printf("Limiting process %ld\n", (long)child_pid);
    }
    /* Command mode runs once, so no scan-failure streak precedes it. */
    limit_status = limit_process(child_pid, cfg->cpu_limit,
                                 cfg->include_children, cfg->verbose, 0);

    /* Resume unconditionally: a stopped member may be invisible. */
    signal_command(child_pid, SIGCONT, cfg->verbose);

    /* Forward the received signal once so the child reports 128 + signal. */
    forwarded_quit_signal = is_quit_flag_set();
    if (forwarded_quit_signal) {
        forward_quit_signal(child_pid, cfg->verbose);
    }

    /* Reap the child either way; a limit failure must not read as success. */
    /* NO_TARGET is the exception: the command's own status is reported. */
    if (limit_status != LIMIT_PROCESS_OK &&
        limit_status != LIMIT_PROCESS_NO_TARGET) {
        /* On a failed limit, report the child's status and the remedy. */
        int child_exit_status =
            collect_child_exit_status(child_pid, cfg, forwarded_quit_signal);
        if (limit_status == LIMIT_PROCESS_SCAN_FAILED ||
            limit_status == LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED) {
            fprintf(
                stderr,
                "Warning: CPU limiting stopped early for process %ld; the command ran unthrottled from that point and exited with status %d\n",
                (long)child_pid, child_exit_status);
        } else if (limit_status == LIMIT_PROCESS_STRANDED) {
            fprintf(
                stderr,
                "Warning: CPU limiting ran for process %ld, but one or more processes it suspended were left stopped; recover each with 'kill -CONT <pid>'. The command exited with status %d\n",
                (long)child_pid, child_exit_status);
        } else {
            fprintf(
                stderr,
                "Warning: CPU limit could not be applied to process %ld; the command exited with status %d\n",
                (long)child_pid, child_exit_status);
        }
        return EXIT_FAILURE;
    }
    return collect_child_exit_status(child_pid, cfg, forwarded_quit_signal);
}

/**
 * @def TARGET_NOT_FOUND
 * @brief Nothing on the system matches the target.
 */
#define TARGET_NOT_FOUND 0

/**
 * @def TARGET_UNCONTROLLABLE
 * @brief The target exists but refuses to be signalled.
 */
#define TARGET_UNCONTROLLABLE 1

/**
 * @def TARGET_RESOLVED
 * @brief A usable PID was written to the out parameter.
 */
#define TARGET_RESOLVED 2

/**
 * @brief Locate the target and classify what the lookup produced.
 *
 * @param cfg Configuration naming the target.
 * @param pid_mode Non-zero when cfg->target_pid selects the target.
 * @param lazy_mode Effective laziness of this run.
 * @param found_pid Out: the PID found, negated for TARGET_UNCONTROLLABLE.
 * @return TARGET_RESOLVED, TARGET_NOT_FOUND or TARGET_UNCONTROLLABLE.
 */
static int resolve_target(const struct cpulimit_cfg *cfg, int pid_mode,
                          int lazy_mode, pid_t *found_pid) {
    *found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                          : find_process_by_name(cfg->exe_name);
    if (*found_pid == 0) {
        if (pid_mode) {
            fprintf(stderr, "Process with PID %ld cannot be found\n",
                    (long)cfg->target_pid);
        } else if (lazy_mode || cfg->verbose) {
            fprintf(stderr, "Process '%s' cannot be found\n", cfg->exe_name);
        }
        return TARGET_NOT_FOUND;
    }
    if (*found_pid < 0) {
        if (lazy_mode || cfg->verbose) {
            fprintf(stderr, "No permission to control process %ld%s\n",
                    -(long)*found_pid, lazy_mode ? "" : ", retrying...");
        }
        return TARGET_UNCONTROLLABLE;
    }
    return TARGET_RESOLVED;
}

/**
 * @brief Record that a resolved PID now runs a different program.
 *
 * @param cfg Configuration naming the target.
 * @param lazy_mode Effective laziness of this run.
 * @param found_pid The PID whose name no longer matches the target.
 * @param exit_status In/out: the run's exit status.
 */
static void handle_stale_target(const struct cpulimit_cfg *cfg, int lazy_mode,
                                pid_t found_pid, int *exit_status) {
    if (lazy_mode || cfg->verbose) {
        fprintf(stderr, "Process %ld is no longer '%s'; not limiting it\n",
                (long)found_pid, cfg->exe_name);
    }
    if (lazy_mode) {
        *exit_status = EXIT_FAILURE;
    }
}

/**
 * @brief Limit one resolved target and leave it running afterwards.
 *
 * @param cfg Configuration naming the target.
 * @param lazy_mode Effective laziness of this run.
 * @param found_pid PID that resolved and proved still to be the target.
 * @param exit_status In/out: the run's exit status.
 * @param scan_failures In/out: consecutive scan-failure streak.
 */
static void limit_and_resume_target(const struct cpulimit_cfg *cfg,
                                    int lazy_mode, pid_t found_pid,
                                    int *exit_status,
                                    unsigned int *scan_failures) {
    int limit_status;
    int pid_reused = 0;
    double target_start_time = UNKNOWN_START_TIME;
    double current_start;

    if (cfg->verbose) {
        printf("Process %ld found\n", (long)found_pid);
    }

    /* Record the start time so the closing SIGCONT can detect PID reuse. */
    target_start_time = get_process_start_time(found_pid);

    /* Pass the streak so a retry does not repeat the scan diagnostic. */
    limit_status =
        limit_process(found_pid, cfg->cpu_limit, cfg->include_children,
                      cfg->verbose, *scan_failures);

    /* Resume unless the PID was recycled while limit_process() ran. */
    /* An unreadable start time still sends SIGCONT to avoid stranding it. */
    current_start = get_process_start_time(found_pid);
    pid_reused = (!start_time_matches(target_start_time, UNKNOWN_START_TIME) &&
                  !start_time_matches(current_start, UNKNOWN_START_TIME) &&
                  !start_time_matches(current_start, target_start_time));
    if (pid_reused) {
        if (cfg->verbose) {
            fprintf(stderr,
                    "Process %ld is no longer the target; not resuming it\n",
                    (long)found_pid);
        }
    } else if (kill(found_pid, SIGCONT) != 0 && errno != ESRCH) {
        int err = errno;
        if (cfg->verbose) {
            fprintf(stderr, "kill(%ld, SIGCONT) failed: %s\n", (long)found_pid,
                    strerror(err));
        }
    }

    if (limit_status == LIMIT_PROCESS_OK) {
        *scan_failures = 0;
    }

    /* A failed scan ends a lazy run but only an attempt of a watching run. */
    if (limit_status == LIMIT_PROCESS_SCAN_FAILED && !lazy_mode) {
        (*scan_failures)++;
    } else if (limit_status != LIMIT_PROCESS_OK &&
               (limit_status != LIMIT_PROCESS_NO_TARGET || lazy_mode)) {
        if (limit_status == LIMIT_PROCESS_SCAN_FAILED) {
            /* A silent lazy run must report its own scan failure. */
            if (!cfg->verbose) {
                fprintf(
                    stderr,
                    "Process group scan failed; CPU limiting stopped for PID %ld, the target is no longer limited\n",
                    (long)found_pid);
            }
        } else if (limit_status == LIMIT_PROCESS_NO_TARGET) {
            /* A lazy NO_TARGET run has no other diagnostic. */
            fprintf(stderr,
                    "Process %ld is no longer running; nothing was limited\n",
                    (long)found_pid);
        }
        *exit_status = EXIT_FAILURE;
    }
}

/**
 * @brief Find and limit an existing process by PID or executable name.
 *
 * @param cfg Configuration naming the target.
 * @return The process exit status.
 *
 * Lazy mode makes one attempt. Otherwise the run keeps re-resolving and
 * re-limiting until a group-building failure or a stranded member ends it;
 * a scan failure after the group is built is reported once per streak and
 * retried. cpulimit itself is never a target.
 *
 * @note A PID target always runs in lazy mode, even without -z.
 */
int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg) {
    const struct timespec wait_time = {2, 0};
    int pid_mode = cfg->target_pid > 0, exit_status = EXIT_SUCCESS;
    /* A PID names one process; waiting for its return could limit another. */
    int lazy_mode = cfg->lazy_mode || pid_mode;
    /* Consecutive scan failures keep the diagnostic to one line per streak. */
    unsigned int scan_failures = 0;

    /* Non-lazy mode keeps re-resolving the target for as long as it runs. */
    while (!is_quit_flag_set()) {
        pid_t found_pid;
        int resolved = resolve_target(cfg, pid_mode, lazy_mode, &found_pid);

        if (resolved == TARGET_UNCONTROLLABLE || resolved == TARGET_NOT_FOUND) {
            if (lazy_mode) {
                exit_status = EXIT_FAILURE;
            }
        } else if (found_pid == getpid()) {
            /* Never limit cpulimit itself. */
            fprintf(stderr, "Error: target process %ld is cpulimit itself\n",
                    (long)found_pid);
            return EXIT_FAILURE;
        } else if (!pid_mode &&
                   process_has_other_name(found_pid, cfg->exe_name)) {
            /* -e may have matched a recycled PID. */
            handle_stale_target(cfg, lazy_mode, found_pid, &exit_status);
        } else {
            limit_and_resume_target(cfg, lazy_mode, found_pid, &exit_status,
                                    &scan_failures);
        }

        if (lazy_mode || is_quit_flag_set() || exit_status != EXIT_SUCCESS) {
            break;
        }

        /* One sleep for the whole interval; a signal ends it early. */
        sleep_timespec(&wait_time);
    }
    return exit_status;
}
