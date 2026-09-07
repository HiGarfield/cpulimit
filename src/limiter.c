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
#include "script_check.h"
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
 * @param cfg Pointer to configuration structure containing command and options
 *
 * This function implements command execution mode (COMMAND [ARG]...):
 * 1. Forks a child process to execute the specified command
 * 2. Creates a new process group for the child
 * 3. Applies CPU limiting to the command and optionally its descendants
 * 4. Waits for command completion and exits with the child's exit status
 *
 * The parent process monitors the child and handles:
 * - Normal exit (returns child's exit code)
 * - Signal termination (returns 128 + signal number)
 * - Timeout after termination request (sends SIGKILL)
 *
 * @return Exit status code; the caller is responsible for calling exit()
 */
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
    limit_status = limit_process(child_pid, cfg->cpu_limit,
                                 cfg->include_children, cfg->verbose);

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
        (void)collect_child_exit_status(child_pid, cfg,
                                        forwarded_quit_signal);
        return EXIT_FAILURE;
    }
    return collect_child_exit_status(child_pid, cfg, forwarded_quit_signal);
}

/**
 * @brief Search for and limit an existing process by PID or executable name
 * @param cfg Pointer to configuration structure containing target specification
 *
 * This function implements PID/exe search mode (-p PID or -e EXE):
 * 1. Continuously searches for the target process
 * 2. When found, applies CPU limiting
 * 3. Behavior depends on lazy_mode flag:
 *    - lazy_mode=1: Exit when target terminates or cannot be found
 *    - lazy_mode=0: Keep searching and re-attach if target restarts
 *
 * @return Exit status code; the caller is responsible for calling exit()
 */
int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg) {
    /*
     * Wait interval between search attempts when target not found.
     * Uses 2-second delay: {tv_sec=2, tv_nsec=0}.
     */
    const struct timespec wait_time = {2, 0};
    int pid_mode = cfg->target_pid > 0, exit_status = EXIT_SUCCESS;

    while (!is_quit_flag_set()) {
        pid_t found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                                   : find_process_by_name(cfg->exe_name);

        if (found_pid == 0) {
            /* Process does not exist */
            if (pid_mode) {
                fprintf(stderr, "Process with PID %ld cannot be found%s\n",
                        (long)cfg->target_pid,
                        cfg->lazy_mode ? "" : ", retrying...");
            } else {
                fprintf(stderr, "Process '%s' cannot be found%s\n",
                        cfg->exe_name, cfg->lazy_mode ? "" : ", retrying...");
            }
            if (cfg->lazy_mode) {
                /* In lazy mode, missing target is an error condition */
                exit_status = EXIT_FAILURE;
            }
        } else if (found_pid < 0) {
            /*
             * Process exists but cannot be controlled (permission denied).
             * Negative PID indicates EPERM error. No point retrying.
             */
            fprintf(stderr, "No permission to control process %ld\n",
                    -(long)found_pid);
            exit_status = EXIT_FAILURE;
            break;
        } else {
            /* LIMIT_PROCESS_OK, or LIMIT_PROCESS_ERROR if it never started */
            int limit_status;

            /*
             * Sanity check: prevent cpulimit from limiting itself.
             * This could cause system instability or deadlock.
             */
            if (found_pid == getpid()) {
                fprintf(stderr,
                        "Error: target process %ld is cpulimit itself\n",
                        (long)found_pid);
                return EXIT_FAILURE;
            }
            if (cfg->verbose) {
                printf("Process %ld found\n", (long)found_pid);
            }
            /*
             * Apply CPU limiting to the target process.
             * This call blocks until the process terminates or quit flag is
             * set.
             */
            limit_status = limit_process(found_pid, cfg->cpu_limit,
                                         cfg->include_children, cfg->verbose);

            /*
             * Always resume the target after limit_process() returns.
             * limit_process() sends SIGCONT via its process list before
             * returning, but on some platforms (e.g. macOS 10.7) a stopped
             * process may not be visible to the process iterator, leaving it
             * stopped even though limit_process() has exited; and if
             * update_process_set() fails, proc_list is cleared so the
             * cleanup SIGCONT inside limit_process() traverses an empty list
             * and cannot resume a still-stopped target. Sending SIGCONT here
             * unconditionally ensures the target is running when we leave.
             * kill() to an already-exited process returns ESRCH, which is
             * harmless here.
             *
             * This mirrors the symmetric guard already present in
             * run_command_mode() after its limit_process() call.
             */
            if (kill(found_pid, SIGCONT) != 0 && errno != ESRCH) {
                int err = errno;
                fprintf(stderr, "kill(%ld, SIGCONT) failed: %s\n",
                        (long)found_pid, strerror(err));
            }

            if (limit_status != LIMIT_PROCESS_OK) {
                /*
                 * Limiting never engaged for this target.  Stop instead of
                 * retrying: the failure is in setting the group up, so the
                 * next attempt would fail the same way and the loop would
                 * just spin on it.
                 */
                exit_status = EXIT_FAILURE;
                break;
            }
        }

        /*
         * Exit conditions:
         * - lazy_mode: Exit after first attempt (regardless of success)
         * - quit_flag: User requested termination via signal
         */
        if (cfg->lazy_mode || is_quit_flag_set()) {
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
