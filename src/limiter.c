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
 * @brief Execute and monitor a user-specified command with CPU limiting
 *
 * @param cfg Pointer to configuration with command and options
 * @return Exit status code; the caller is responsible for calling exit()
 *
 * Forks a child into its own process group, applies the limit, and waits for
 * completion, sending SIGKILL after a termination-request timeout.
 */
int run_command_mode(const struct cpulimit_cfg *cfg) {
    pid_t child_pid;
    int sync_pipe[2];
    int fd_flags;
    /* 1 when the quit flag is set and the signal must be forwarded */
    int forwarded_quit_signal;
    int limit_status = LIMIT_PROCESS_OK;

    /*
     * The write end is set FD_CLOEXEC so a successful execvp() in the child
     * closes it and signals the parent; on exec failure the child closes it
     * before _exit(). The parent's second read then blocks until exec is
     * done, so it never signals the child mid-exec (matters under valgrind).
     */
    if (pipe(sync_pipe) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    fd_flags = fcntl(sync_pipe[1], F_GETFD);
    if (fd_flags < 0 ||
        fcntl(sync_pipe[1], F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
        perror("fcntl");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return EXIT_FAILURE;
    }

    /* Flush before fork so buffered output is not duplicated at each process's
     * exit. */
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
        /* exec_child_process() never returns */
    }

    close(sync_pipe[1]);
    if (wait_for_child_exec(child_pid, sync_pipe[0]) != 0) {
        return EXIT_FAILURE;
    }

    if (cfg->verbose) {
        printf("Limiting process %ld\n", (long)child_pid);
    }
    /* Command mode runs once, so every scan failure is the first: pass 0. */
    limit_status = limit_process(child_pid, cfg->cpu_limit,
                                 cfg->include_children, cfg->verbose, 0);

    /*
     * Resume unconditionally: limit_process() may have left a stopped member
     * invisible to the iterator (e.g. macOS 10.7), and an already-running or
     * already-exited child ignores SIGCONT safely.
     */
    signal_command(child_pid, SIGCONT);

    /*
     * Forward the exact received signal so the child exits with the status a
     * shell would report (e.g. SIGINT -> 130, not SIGTERM's 143), exactly
     * once: collect_child_exit_status() is told it was delivered. If the
     * signal arrives after this check (a race when a stopped child is
     * invisible to the iterator), that function forwards it from its loop.
     */
    forwarded_quit_signal = is_quit_flag_set();
    if (forwarded_quit_signal) {
        forward_quit_signal(child_pid);
    }

    /*
     * Reap the child either way; when limiting never engaged, discard its
     * status so the run is not reported as a successful limited one.
     */
    if (limit_status != LIMIT_PROCESS_OK &&
        limit_status != LIMIT_PROCESS_NO_TARGET) {
        /*
         * Limit_process() did not limit to completion; surface the child's
         * real exit status so the operator sees why, not just cpulimit's
         * own EXIT_FAILURE. Distinguish the three outcomes -- a group that
         * could not be built at all, a run that went unthrottled from a failed
         * scan, and the stranded case where limit_process() names each
         * 'kill -CONT <pid>' to recover.
         */
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
                "Warning: CPU limiting ran for process %ld, but one or more processes it suspended were left stopped; recover each with the 'kill -CONT <pid>' printed above.  The command exited with status %d\n",
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
 * @brief resolve_target() result: nothing on the system matches the target
 */
#define TARGET_NOT_FOUND 0

/**
 * @def TARGET_UNCONTROLLABLE
 * @brief resolve_target() result: the target exists but refuses to be
 *        signalled, so nothing can be limited right now
 */
#define TARGET_UNCONTROLLABLE 1

/**
 * @def TARGET_RESOLVED
 * @brief resolve_target() result: a usable PID was written to the out
 *        parameter
 */
#define TARGET_RESOLVED 2

/**
 * @brief Locate the target and classify what the lookup produced
 *
 * @param cfg Pointer to the configuration naming the target
 * @param pid_mode Non-zero when cfg->target_pid selects the target, zero when
 *        cfg->exe_name does
 * @param lazy_mode Effective laziness of this run, as resolved by
 *        run_pid_or_exe_mode(): a PID always makes the run lazy
 * @param found_pid Out: the PID the finder reported (negated for
 *        TARGET_UNCONTROLLABLE)
 * @return TARGET_RESOLVED, TARGET_NOT_FOUND or TARGET_UNCONTROLLABLE
 *
 * The PID-mode "cannot be found" diagnostic is printed here because it follows
 * from what the lookup found, not from the caller's policy; non-lazy mode
 * appends ", retrying...". The name-mode "cannot be found" message is printed
 * by find_process_by_name() itself. A name that resolves only to PID 1 is
 * refused once at argument-checking time (cli.c) before the limiter starts, so
 * it is never reported here.
 */
static int resolve_target(const struct cpulimit_cfg *cfg, int pid_mode,
                          int lazy_mode, pid_t *found_pid) {
    *found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                          : find_process_by_name(cfg->exe_name);
    if (*found_pid == 0) {
        /*
         * find_process_by_name() already printed "cannot be found";
         * find_process_by_pid() has nothing to print. PID mode is always one
         * attempt, so this line never has a retry behind it to advertise and
         * the suffix would be dead text.
         */
        if (pid_mode) {
            fprintf(stderr, "Process with PID %ld cannot be found\n",
                    (long)cfg->target_pid);
        }
        return TARGET_NOT_FOUND;
    }
    if (*found_pid < 0) {
        /*
         * The process exists but refuses to be signalled, so nothing can be
         * attached to right now. What that means for the search is the
         * caller's decision, not this function's: a refusal belongs to the
         * process currently wearing that name or PID, and a later one may be
         * controllable.
         */
        fprintf(stderr, "No permission to control process %ld%s\n",
                -(long)*found_pid, lazy_mode ? "" : ", retrying...");
        return TARGET_UNCONTROLLABLE;
    }
    return TARGET_RESOLVED;
}

/**
 * @brief Record that a resolved PID now runs a different program
 *
 * @param cfg Pointer to the configuration naming the target
 * @param lazy_mode Effective laziness of this run, as resolved by
 *        run_pid_or_exe_mode(): a PID always makes the run lazy
 * @param found_pid The PID whose name no longer matches the target
 * @param exit_status In/out: the running exit status of the whole run
 *
 * The resolved process exited and its PID was reused, so this attempt leaves
 * it untouched. Lazy mode ends the run as a failure; non-lazy keeps looking.
 */
static void handle_stale_target(const struct cpulimit_cfg *cfg, int lazy_mode,
                                pid_t found_pid, int *exit_status) {
    fprintf(stderr, "Process %ld is no longer '%s'; not limiting it\n",
            (long)found_pid, cfg->exe_name);
    if (lazy_mode) {
        *exit_status = EXIT_FAILURE;
    }
}

/**
 * @brief Limit one resolved target and leave it running afterwards
 *
 * @param cfg Pointer to the configuration naming the target
 * @param lazy_mode Effective laziness of this run, as resolved by
 *        run_pid_or_exe_mode(): a PID always makes the run lazy
 * @param found_pid PID that resolved and proved still to be the target
 * @param exit_status In/out: the running exit status of the whole run
 * @param scan_failures In/out: consecutive scan-failure streak
 *
 * Blocks inside limit_process(), then resumes the target unless its PID was
 * recycled, and sets *exit_status on the outcomes that end the whole run: a
 * group that could not be built, a stranded member needing a manual 'kill
 * -CONT', and -- in lazy mode, which makes one attempt and reports whatever
 * it produced -- a target that could not be limited at all.
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

    /*
     * Record the start time now so the closing SIGCONT can tell this process
     * from whatever the PID was recycled into. Both modes use it as the
     * authoritative identity: an exec() rewrites the name but not the
     * process, so -e must not fall back to comparing names.
     */
    target_start_time = get_process_start_time(found_pid);

    /*
     * Pass the streak so far: this caller retries, and without it every
     * retry would repeat the same scan diagnostic every two seconds.
     */
    limit_status =
        limit_process(found_pid, cfg->cpu_limit, cfg->include_children,
                      cfg->verbose, *scan_failures);

    /*
     * Resume unless the PID changed hands while limit_process() ran. The
     * SIGCONT is usually redundant (limit_process() already sent one), but not
     * guaranteed: on macOS 10.7 a stopped process is invisible to the
     * iterator, so a failed scan leaves an empty list and no resume. It is
     * conditional because the PID may have been recycled by now, and a blind
     * SIGCONT would resume a process someone else is intentionally holding
     * stopped. Only a differing start time proves the change -- a name is not
     * identity. An unreadable start time means nobody can tell, so we send
     * anyway and avoid stranding a stopped target. Both "unreadable" and
     * "differing" are asked through start_time_matches(), the same comparison
     * the process set uses before it resumes a member, so one definition of
     * "still the same process" decides both.
     */
    current_start = get_process_start_time(found_pid);
    pid_reused = (!start_time_matches(target_start_time, UNKNOWN_START_TIME) &&
                  !start_time_matches(current_start, UNKNOWN_START_TIME) &&
                  !start_time_matches(current_start, target_start_time));
    if (pid_reused) {
        fprintf(stderr,
                "Process %ld is no longer the target; not resuming it\n",
                (long)found_pid);
    } else if (kill(found_pid, SIGCONT) != 0 && errno != ESRCH) {
        int err = errno;
        fprintf(stderr, "kill(%ld, SIGCONT) failed: %s\n", (long)found_pid,
                strerror(err));
    }

    if (limit_status == LIMIT_PROCESS_OK) {
        *scan_failures = 0;
    }

    /*
     * A bad scan ends the run in lazy mode (one attempt, then done) but only
     * ends the attempt in non-lazy mode, which keeps re-resolving and
     * re-attaching -- that is what the watch mode is for. An empty group
     * (LIMIT_PROCESS_NO_TARGET) says nothing about the search either: it is
     * what a target that turned into a zombie between the lookup and the
     * first scan looks like, and such a target may be restarted as a process
     * this run can limit, so non-lazy keeps watching and only lazy mode ends
     * on it.
     */
    if (limit_status == LIMIT_PROCESS_SCAN_FAILED && !lazy_mode) {
        (*scan_failures)++;
    } else if (limit_status != LIMIT_PROCESS_OK &&
               (limit_status != LIMIT_PROCESS_NO_TARGET || lazy_mode)) {
        /*
         * Everything here ends the run: a group that could not be built
         * (LIMIT_PROCESS_ERROR, the one reason a non-lazy search gives up),
         * or a stranded outcome needing a manual 'kill -CONT' to recover.
         */
        *exit_status = EXIT_FAILURE;
    }
}

/**
 * @brief Search for and limit an existing process by PID or executable name
 *
 * @param cfg Pointer to configuration with the target specification
 * @return Exit status code; the caller is responsible for calling exit()
 *
 * In lazy mode one attempt is made and the program exits on any non-target
 * exit. Otherwise it watches until a scan failure or a stranded member stops
 * it; a target reappearing on a recycled PID is re-limited. cpulimit itself is
 * always refused as a target.
 *
 * @note A PID target always runs in lazy mode, whether or not the
 *       configuration asked for it: the number identifies a single process,
 *       so searching for it to come back would limit an unrelated one.
 */
int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg) {
    const struct timespec wait_time = {2, 0};
    int pid_mode = cfg->target_pid > 0, exit_status = EXIT_SUCCESS;
    /*
     * A PID names one process, not a class of processes. Once that process is
     * gone the number may be handed to an unrelated one, and a run that kept
     * searching would go on to limit whoever picked it up -- the opposite of
     * what -p asked for. So a PID makes the run a single attempt whatever the
     * configuration says, and this is the one place that decides it: the
     * option parser setting lazy_mode for -p is a helpful second line of
     * defence, not the guarantee. Anything that reaches this function with a
     * target_pid therefore ends up lazy by construction.
     */
    int lazy_mode = cfg->lazy_mode || pid_mode;
    /*
     * Consecutive scan failures: limit_process() prints the diagnostic once
     * per streak, so this keeps a retrying run from repeating the line every
     * two seconds. Reset when a run limits to completion.
     */
    unsigned int scan_failures = 0;

    /*
     * Non-lazy mode is a watch: it keeps resolving and re-attaching for as
     * long as the run lasts, because the target can always come back (not
     * started yet, exited, restarted on a recycled PID, or refusing signals).
     * Only a scanning-machinery failure or a stranded member ends it. Lazy
     * mode is the opposite: one attempt, whatever it produced. Reaching here
     * with lazy_mode set is how a PID-target run behaves; the name-target run
     * without -z is the watch.
     */
    while (!is_quit_flag_set()) {
        pid_t found_pid;
        int resolved = resolve_target(cfg, pid_mode, lazy_mode, &found_pid);

        if (resolved == TARGET_UNCONTROLLABLE || resolved == TARGET_NOT_FOUND) {
            /*
             * Nothing to attach to right now. Lazy mode ends with a failure;
             * non-lazy keeps looking, because the target may yet start or be
             * restarted as a process this run can limit.
             */
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
            /* -e may have matched a recycled PID now running a different
             * program. */
            handle_stale_target(cfg, lazy_mode, found_pid, &exit_status);
        } else {
            limit_and_resume_target(cfg, lazy_mode, found_pid, &exit_status,
                                    &scan_failures);
        }

        if (lazy_mode || is_quit_flag_set() || exit_status != EXIT_SUCCESS) {
            break;
        }

        /*
         * One sleep for the whole interval: sleep_timespec() returns early on a
         * termination signal instead of resuming the remainder, so the quit
         * flag is re-checked right away without slicing the wait.
         */
        sleep_timespec(&wait_time);
    }
    return exit_status;
}
