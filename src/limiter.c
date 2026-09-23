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

/*
 * Number of "target not found" retries allowed in non-lazy mode before
 * cpulimit gives up.  See MAX_TARGET_LOOKUP_ATTEMPTS usage in
 * run_pid_or_exe_mode: a target that never appears should not be retried
 * forever.  Fifteen attempts at the two-second wait below is thirty seconds
 * of grace for a target that is slow to start, after which giving up is the
 * only sane outcome.
 */
#define MAX_TARGET_LOOKUP_ATTEMPTS 15

#include "limiter.h"

#include "child_exec.h"
#include "child_wait.h"
#include "cli.h"
#include "exec_sync.h"
#include "limit_process.h"
#include "process_finder.h"
#include "process_iterator.h"
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

int run_command_mode(const struct cpulimit_cfg *cfg) {
    /* PID of forked child that will execute the command */
    pid_t child_pid;
    /* Pipe for parent-child synchronization */
    int sync_pipe[2];
    /* Current file descriptor flags for sync_pipe[1] */
    int fd_flags;
    /* 1 when the quit flag is set and the signal must be forwarded */
    int forwarded_quit_signal;
    /* LIMIT_PROCESS_OK, or LIMIT_PROCESS_ERROR if limiting never started */
    int limit_status = LIMIT_PROCESS_OK;

    /*
     * Create pipe for synchronization.
     * The write end (sync_pipe[1]) has its close-on-exec flag set (FD_CLOEXEC)
     * so that a successful execvp() in the child closes it automatically,
     * signalling the parent that exec has completed.  On exec failure the
     * child closes it explicitly before _exit().  This lets the parent
     * perform a second read that blocks until exec is done (or the child
     * has exited), which ensures we never send signals to the child while
     * it is in the middle of exec setup (critical for correct behaviour
     * under tools such as valgrind that intercept execve).
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

    /*
     * Flush stdout and stderr before forking.
     * This is a defensive measure to avoid duplicated buffered output:
     * after fork() both parent and child inherit unflushed stdio buffers,
     * so any pending output would be flushed by both processes at their
     * respective exit() calls. stderr may be line-buffered when redirected
     * to a file, making this flush necessary in automated environments.
     */
    fflush(stdout);
    fflush(stderr);

    /* Fork to create child process that will execute user command */
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

    /* Parent: close unused write end before waiting for child */
    close(sync_pipe[1]);
    if (wait_for_child_exec(child_pid, sync_pipe[0]) != 0) {
        return EXIT_FAILURE;
    }

    /*
     * Apply CPU limiting to child process.
     * If include_children is set, limit_process() also tracks and limits
     * all descendant processes. This call blocks until child terminates
     * or a quit signal is received.
     */
    if (cfg->verbose) {
        printf("Limiting process %ld\n", (long)child_pid);
    }
    /* Command mode runs once, so every scan failure is the first: pass 0. */
    limit_status = limit_process(child_pid, cfg->cpu_limit,
                                 cfg->include_children, cfg->verbose, 0);

    /*
     * Always resume the process group after limit_process() returns.
     * limit_process() sends SIGCONT via its process list before returning,
     * but on some platforms (e.g. macOS 10.7) a stopped process may not be
     * visible to the process iterator, leaving it stopped even though
     * limit_process() has exited.  Sending SIGCONT unconditionally here
     * ensures the child is running and able to receive any subsequent signal.
     * SIGCONT to an already-running process group is harmless.
     * A child that has already exited cannot be resumed; that case is
     * handled by collect_child_exit_status() below.
     */
    signal_command(child_pid, SIGCONT);

    /*
     * Check if user requested termination via signal (Ctrl+C, SIGTERM,
     * etc). If so, gracefully terminate the entire process group by
     * forwarding the exact received signal, so the child exits with the
     * status a shell would report: for example, Ctrl+C (SIGINT) is
     * forwarded as SIGINT so the child exits with status 130
     * (128+SIGINT), not 143 (128+SIGTERM).
     *
     * The child must receive this signal exactly once, so
     * collect_child_exit_status() is told that it has been delivered and
     * must not repeat it.
     *
     * Note: if the quit signal arrives after this check (a race on
     * platforms where limit_process() exits early because a stopped
     * process is invisible to the iterator), collect_child_exit_status()
     * will detect and forward it from inside its polling loop.
     */
    forwarded_quit_signal = is_quit_flag_set();
    if (forwarded_quit_signal) {
        forward_quit_signal(child_pid);
    }

    /*
     * Reap the child either way.  When limiting never started this is what
     * keeps the command from being abandoned: the child was resumed above
     * and is waited for here, so it never outlives cpulimit unaccounted
     * for.  Its own status is then discarded, because a run in which the
     * limiter never engaged must not be reported as a successful limited
     * run.
     */
    if (limit_status != LIMIT_PROCESS_OK) {
        /*
         * The command child was not limited to completion either way; surface
         * its real exit status so the operator can diagnose the outcome
         * instead of only seeing cpulimit's own EXIT_FAILURE.
         *
         * The two reasons are different and must not be reported as one.
         * LIMIT_PROCESS_ERROR means the group was never built and nothing
         * was limited at all.  LIMIT_PROCESS_SCAN_FAILED means limiting did
         * run and only stopped when a scan failed, so the command merely ran
         * unthrottled from that point on: saying the limit "could not be
         * applied" sends anyone debugging it after permissions or target
         * resolution instead of the failed scan.
         *
         * LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED is both at once, so it takes
         * the scan wording: limiting did run and stopped early, which is the
         * part the command's own status cannot show.  The stranded members
         * are not mentioned here because limit_process() has already named
         * each one with the 'kill -CONT <pid>' that recovers it.
         */
        int child_exit_status =
            collect_child_exit_status(child_pid, cfg, forwarded_quit_signal);
        if (limit_status == LIMIT_PROCESS_SCAN_FAILED ||
            limit_status == LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED) {
            fprintf(
                stderr,
                "Warning: CPU limiting stopped early for process %ld; the command ran unthrottled from that point and exited with status %d\n",
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
 * @def STREAK_LOOKUP
 * @brief Failure streak passed to bump_retry_streak() when searching for the
 *        target did not produce a usable PID
 */
#define STREAK_LOOKUP 0

/**
 * @def STREAK_SCAN
 * @brief Failure streak passed to bump_retry_streak() when scanning the
 *        process group failed while the control loop was running
 */
#define STREAK_SCAN 1

/**
 * @brief Count one consecutive failure and report when its cap is reached
 * @param count Pointer to the consecutive failure streak for this kind
 * @param kind STREAK_LOOKUP or STREAK_SCAN; selects the diagnostic printed
 * @param exit_status Pointer to the running exit status; set to EXIT_FAILURE
 *        when the cap is reached
 * @return 1 when the cap has just been reached and the caller should stop,
 *         0 while there is still budget for another attempt
 *
 * Every retry in the search loop is capped for the same reason: a target
 * that never appears, a name that keeps resolving to a recycled PID, and a
 * process table that cannot be scanned at all are conditions that waiting
 * cannot fix, so the loop has to end eventually instead of printing two
 * diagnostic lines every two seconds for as long as cpulimit runs. The
 * streak counts consecutive failures rather than lifetime ones, so a daemon
 * that restarts periodically keeps being re-attached instead of exhausting
 * a budget that was only ever meant to bound one wait.
 *
 * Whether reaching the cap ends the loop stays the caller's decision, which
 * is what lets the three call sites keep the control flow they had: the two
 * target-lookup paths fall through to the loop's own exit check below, while
 * a failing scan breaks out at once because nothing in this iteration can
 * still be retried.
 */
static int bump_retry_streak(unsigned int *count, int kind, int *exit_status) {
    (*count)++;
    if (*count < MAX_TARGET_LOOKUP_ATTEMPTS) {
        return 0;
    }
    if (kind == STREAK_SCAN) {
        fprintf(
            stderr,
            "Giving up after %u failed scan(s): the target is no longer limited\n",
            *count);
    } else {
        fprintf(stderr, "Giving up after %u attempts: target not found\n",
                *count);
    }
    *exit_status = EXIT_FAILURE;
    return 1;
}

/**
 * @def TARGET_NOT_FOUND
 * @brief resolve_target() result: nothing on the system matches the target
 */
#define TARGET_NOT_FOUND 0

/**
 * @def TARGET_UNCONTROLLABLE
 * @brief resolve_target() result: the target exists but refuses to be
 *        signalled, so no retry can ever succeed
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
 * @param cfg Pointer to the configuration naming the target
 * @param pid_mode Non-zero when cfg->target_pid selects the target, zero when
 *        cfg->exe_name does
 * @param found_pid Out: the PID the finder reported. For TARGET_RESOLVED it
 *        is the target; for TARGET_UNCONTROLLABLE it is that PID negated
 * @return TARGET_RESOLVED, TARGET_NOT_FOUND or TARGET_UNCONTROLLABLE
 *
 * Both failure diagnostics are printed here rather than by the caller,
 * because they follow from what the lookup found and not from what the
 * caller then decides to do about it: every policy reports them the same.
 */
static int resolve_target(const struct cpulimit_cfg *cfg, int pid_mode,
                          pid_t *found_pid) {
    *found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                          : find_process_by_name(cfg->exe_name);
    if (*found_pid == 0) {
        if (pid_mode) {
            fprintf(stderr, "Process with PID %ld cannot be found%s\n",
                    (long)cfg->target_pid,
                    cfg->lazy_mode ? "" : ", retrying...");
        } else {
            fprintf(stderr, "Process '%s' cannot be found%s\n", cfg->exe_name,
                    cfg->lazy_mode ? "" : ", retrying...");
        }
        return TARGET_NOT_FOUND;
    }
    if (*found_pid < 0) {
        /*
         * The process exists but signalling it is refused. There is nothing
         * to attach to and no point retrying, so the caller stops here.
         */
        fprintf(stderr, "No permission to control process %ld\n",
                -(long)*found_pid);
        return TARGET_UNCONTROLLABLE;
    }
    return TARGET_RESOLVED;
}

/**
 * @brief Record that a resolved PID now runs a different program
 * @param cfg Pointer to the configuration naming the target
 * @param found_pid The PID whose name no longer matches the target
 * @param exit_status In/out: the running exit status of the whole run
 * @param lookup_attempts In/out: consecutive target-lookup failure streak
 *
 * The process we resolved has exited and its PID has been reused, so this
 * attempt deliberately does not touch it: suspending it would suspend an
 * unrelated program and resuming it would be equally wrong.
 *
 * Lazy mode ends the run reporting failure, because a run that never limited
 * anything must not come back as success. Non-lazy mode counts the attempt
 * against the same cap as a missing target - a stale name is the same kind of
 * never-arriving target - and starts the search over for the real one.
 */
static void handle_stale_target(const struct cpulimit_cfg *cfg, pid_t found_pid,
                                int *exit_status,
                                unsigned int *lookup_attempts) {
    fprintf(stderr, "Process %ld is no longer '%s'; not limiting it\n",
            (long)found_pid, cfg->exe_name);
    if (cfg->lazy_mode) {
        *exit_status = EXIT_FAILURE;
        return;
    }
    (void)bump_retry_streak(lookup_attempts, STREAK_LOOKUP, exit_status);
}

/**
 * @brief Limit one resolved target and leave it running afterwards
 * @param cfg Pointer to the configuration naming the target
 * @param found_pid PID that resolved and proved to still be the target
 * @param exit_status In/out: the running exit status of the whole run
 * @param scan_failures In/out: consecutive process-group scan failure streak
 *
 * Blocks inside limit_process() for the life of the run, then resumes the
 * target and decides whether the search loop should end.
 *
 * Ending the loop is expressed by setting *exit_status, not by returning a
 * verdict: every outcome that stops the run also fails it, and the caller's
 * exit check sees that in the same iteration with nothing in between, so the
 * effect is the same as ending the loop from in here.
 */
static void limit_and_resume_target(const struct cpulimit_cfg *cfg,
                                    pid_t found_pid, int *exit_status,
                                    unsigned int *scan_failures) {
    /* LIMIT_PROCESS_OK, or LIMIT_PROCESS_ERROR if it never started. */
    int limit_status;
    /* Set when this PID is shown to no longer be our target. */
    int pid_reused = 0;
    /* Start time before limit_process(); both -p and -e use it to detect
     * PID recycling. */
    double target_start_time = UNKNOWN_START_TIME;
    double current_start;

    if (cfg->verbose) {
        printf("Process %ld found\n", (long)found_pid);
    }

    /*
     * Recorded before limiting so the closing SIGCONT below can tell this
     * process from whatever the PID may have been recycled into while
     * limit_process() was running.  Both -p and -e use the start time
     * because it is the authoritative identity; an exec() changes
     * the name but not the process, so the -e branch must not fall back to
     * comparing names or a re-exec'd target would be stranded.
     */
    target_start_time = get_process_start_time(found_pid);

    /*
     * Apply CPU limiting to the target process.  This call blocks until the
     * process terminates or the quit flag is set.
     */
    /*
     * Pass the streak so far: this caller retries, and without it every
     * retry would repeat the same scan diagnostic every two seconds.
     */
    limit_status =
        limit_process(found_pid, cfg->cpu_limit, cfg->include_children,
                      cfg->verbose, *scan_failures);

    /*
     * Always resume the target after limit_process() returns.
     * limit_process() sends SIGCONT via its process list before returning,
     * but on some platforms (e.g. macOS 10.7) a stopped process may not be
     * visible to the process iterator, leaving it stopped even though
     * limit_process() has exited; and if update_process_set() fails,
     * proc_list is cleared so the cleanup SIGCONT inside limit_process()
     * traverses an empty list and cannot resume a still-stopped target.
     * Sending SIGCONT here unconditionally ensures the target is running when
     * we leave.  kill() to an already-exited process returns ESRCH, which is
     * harmless here.
     *
     * This mirrors the symmetric guard already present in run_command_mode()
     * after its limit_process() call.
     *
     * It is only unconditional while this PID is still the target.
     * limit_process() blocks for a long time, and by the time it returns the
     * PID may have been recycled, so an unconditional SIGCONT can resume a
     * process that somebody else is holding stopped on purpose: job control,
     * a debugger, another cpulimit instance.  The signal is therefore skipped
     * only when the PID can be shown to have changed hands: whichever
     * mode, when its start time differs from the one recorded above.  The
     * start time is the authoritative identity; the executable name
     * is not an identity signal, because an exec() changes argv[0] without
     * changing the process, so a re-exec'd target must not be mistaken for a
     * hand-off.  A start time the platform cannot report means nobody
     * can tell, so the signal is sent anyway: stranding a stopped target is
     * precisely what this fallback exists to prevent.
     */
    current_start = get_process_start_time(found_pid);
    /*
     * Relational comparisons only: -Wfloat-equal rejects ==/!= on doubles,
     * and a real start time is positive while UNKNOWN_START_TIME is not.
     * The name is deliberately ignored.
     */
    pid_reused = (target_start_time > 0.0 && current_start > 0.0 &&
                  (current_start < target_start_time ||
                   current_start > target_start_time));
    if (pid_reused) {
        /*
         * Unconditional now: a silently skipped resume strands the target
         * forever, far worse than the harmless SIGCONT we avoided.
         */
        fprintf(stderr,
                "Process %ld is no longer the target; not resuming it\n",
                (long)found_pid);
    } else if (kill(found_pid, SIGCONT) != 0 && errno != ESRCH) {
        int err = errno;
        fprintf(stderr, "kill(%ld, SIGCONT) failed: %s\n", (long)found_pid,
                strerror(err));
    }

    /*
     * A run that limited to completion ends the streak, the same way a
     * resolved target ends the not-found streak.
     */
    if (limit_status == LIMIT_PROCESS_OK) {
        *scan_failures = 0;
    }

    /*
     * Whether a bad scan that stopped the control loop counts as a failure
     * depends on whether there is a second chance: non-lazy mode re-resolves
     * the target on every iteration, so for it falling through is exactly the
     * retry that is wanted.  Lazy mode ends after one attempt, so a limit
     * that stopped there is final: the target is no longer limited and
     * nothing will re-attach to it, the same outcome command mode already
     * reports as a failure.  limit_process() has already said why on stderr
     *.
     */
    if (limit_status == LIMIT_PROCESS_SCAN_FAILED && !cfg->lazy_mode) {
        /*
         * The retry is bounded, and the bound exists for the same reason as
         * the not-found one: a scan that keeps failing is not a
         * target that will come back, it is an environment that cannot be
         * scanned at all (no procfs, sustained allocation pressure).  Left
         * unbounded it would re-walk the whole process table every two
         * seconds, print a diagnostic each time and never exit.  Fifteen
         * attempts is thirty seconds of grace for a transient failure.
         *
         * A target that simply is not there is a different case and is
         * handled by the caller: that wait stays open ended, because a
         * daemon that starts late is exactly what non-lazy mode promises to
         * wait for.
         */
        (void)bump_retry_streak(scan_failures, STREAK_SCAN, exit_status);
    } else if (limit_status != LIMIT_PROCESS_OK) {
        /*
         * Limiting never engaged for this target, or it ran and then stopped
         * with no second chance left.  Stop instead of retrying: the failure
         * is in setting the group up, so the next attempt would fail the same
         * way and the loop would just spin on it.
         */
        *exit_status = EXIT_FAILURE;
    }
}

int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg) {
    /* Wait interval between search attempts: two seconds. */
    const struct timespec wait_time = {2, 0};
    int pid_mode = cfg->target_pid > 0, exit_status = EXIT_SUCCESS;
    /*
     * Consecutive target-lookup failures, non-lazy mode.  Without a cap the
     * loop printed "retrying..." forever, so a name that can never match or
     * a process that never starts would spin indefinitely; fifteen attempts
     * at two seconds each is thirty seconds of grace for a slow target.
     *
     * Consecutive rather than lifetime: a daemon that restarts
     * periodically keeps being re-attached instead of exhausting a budget
     * that was only ever meant to bound one wait.
     *
     * unsigned: the increment followed by the bound check folds into
     * "X + 1 >= C", which -Wstrict-overflow=5 reads as assuming signed
     * overflow cannot happen.
     */
    unsigned int lookup_attempts = 0;
    /*
     * Consecutive scan failures inside the control loop, non-lazy mode.
     * Deliberately not lookup_attempts: that one is reset every time the
     * target resolves, which happens on every retry here, so reusing it
     * could never reach the cap and a scan that keeps failing would be
     * retried forever.  It is reset only when a run actually limits to
     * completion, so a target whose scanning fails occasionally keeps its
     * full budget.
     */
    unsigned int scan_failures = 0;

    while (!is_quit_flag_set()) {
        pid_t found_pid;
        int resolved = resolve_target(cfg, pid_mode, &found_pid);

        if (resolved == TARGET_UNCONTROLLABLE) {
            exit_status = EXIT_FAILURE;
            break;
        }
        if (resolved == TARGET_NOT_FOUND) {
            if (cfg->lazy_mode) {
                /* In lazy mode, missing target is an error condition */
                exit_status = EXIT_FAILURE;
            } else {
                /* Non-lazy mode waits for a slow target, but not forever
                 *: cap the attempts so the run ends instead of
                 * looping and growing stderr without limit.  The cap exits
                 * the loop through the check below. */
                (void)bump_retry_streak(&lookup_attempts, STREAK_LOOKUP,
                                        &exit_status);
            }
        } else if (found_pid == getpid()) {
            /*
             * Never limit this process: that would deadlock or destabilise
             * the system.  Returning outright rather than ending the loop
             * keeps a later path from resuming what this refused to touch.
             */
            fprintf(stderr, "Error: target process %ld is cpulimit itself\n",
                    (long)found_pid);
            return EXIT_FAILURE;
        } else if (!pid_mode &&
                   process_has_other_name(found_pid, cfg->exe_name)) {
            /*
             * Only -e can go stale: it resolves by name, so the PID may
             * have been recycled.  -p names the PID explicitly and that
             * choice is never second-guessed.
             */
            handle_stale_target(cfg, found_pid, &exit_status, &lookup_attempts);
        } else {
            /*
             * A resolved, non-stale target ends the streak: the caps
             * bound one wait, not the process lifetime, so a daemon that
             * restarts daily stays attached across restarts.
             */
            lookup_attempts = 0;
            limit_and_resume_target(cfg, found_pid, &exit_status,
                                    &scan_failures);
        }

        /*
         * Exit conditions:
         * - lazy_mode: Exit after first attempt (regardless of success)
         * - quit_flag: User requested termination via signal
         */
        if (cfg->lazy_mode || is_quit_flag_set() ||
            exit_status != EXIT_SUCCESS) {
            break;
        }

        /*
         * In non-lazy mode, wait before retrying.
         * This prevents excessive CPU usage when target is not running.
         */
        sleep_timespec(&wait_time);
    }
    return exit_status;
}
