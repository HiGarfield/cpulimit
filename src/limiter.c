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
#include "cli.h"
#include "limit_process.h"
#include "process_finder.h"
#include "script_check.h"
#include "signal_forward.h"
#include "signal_handler.h"
#include "time_util.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

/**
 * @brief Wait for the child process to complete exec setup
 * @param child_pid PID of the forked child process
 * @param sync_read_fd Read end of the synchronization pipe
 *
 * Reads the readiness byte written by the child after setpgid() and signal
 * handler reset, then blocks until the pipe EOF that indicates exec has
 * completed (or the child has exited on exec failure). Closes sync_read_fd
 * on return.
 *
 * On any error, kills and reaps the child, then returns -1 so the caller
 * can decide how to terminate.
 *
 * @return 0 on success, -1 on error
 */
static int wait_for_child_exec(pid_t child_pid, int sync_read_fd) {
    /* Synchronization byte from child */
    char sync_byte;
    /* Bytes read from pipe */
    ssize_t n_read;

    /*
     * Block until child signals readiness by writing to pipe.
     * This ensures child has completed setpgid() before parent continues.
     */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    if (n_read != 1 || sync_byte != 'A') {
        /* Return value of waitpid in error path */
        pid_t wait_result;
        if (n_read < 0) {
            perror("read sync");
        } else if (n_read == 0) {
            fprintf(stderr, "Synchronization pipe closed before child setup\n");
        } else {
            fprintf(stderr, "Unexpected synchronization value from child\n");
        }
        close(sync_read_fd);
        /*
         * Kill child to prevent it from becoming an orphan, then reap
         * it to prevent zombie. SIGKILL is used because the child may
         * have failed in an unknown state and cannot be trusted to
         * respond to SIGTERM. SIGKILL cannot be caught or ignored, so
         * the subsequent blocking waitpid() will complete quickly.
         */
        kill(child_pid, SIGKILL);
        /*
         * Robustly reap the child: retry waitpid() on EINTR so that
         * a signal delivered to the parent during the wait does not
         * leave the child as a zombie. For any other error, log it and
         * proceed with the fatal exit.
         */
        do {
            wait_result = waitpid(child_pid, NULL, 0);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == -1) {
            perror("waitpid");
        }
        return -1;
    }
    /*
     * Drain the sync pipe until EOF to confirm the child has closed its
     * write end.  On a successful exec, FD_CLOEXEC closes the write end
     * automatically; on exec failure the child closes it explicitly
     * before _exit().  Only EOF (n_read == 0) guarantees the write end
     * is closed and the child will not write again.
     *
     * Reading just one byte and then closing the read end is unsafe:
     * if the caller has closed fds 1 and 2 before this function is
     * called, pipe() assigns those fds to the sync pipe, causing the
     * child's FILE *stderr (fd 2) to alias the sync pipe write end.
     * perror() in the exec-failure path then makes multiple write()
     * calls to fd 2.  Closing the read end after only one byte leaves
     * the write end still open; the child's next write() receives
     * SIGPIPE (default action: terminate), so the child dies from a
     * signal instead of calling _exit(127), producing exit code 141
     * (128+SIGPIPE) rather than 127.
     *
     * Draining until EOF keeps the read end open for all of the child's
     * writes, eliminating the SIGPIPE race.  It also eliminates the race
     * where a signal (e.g. SIGTERM) is sent while the child is still in
     * the middle of exec setup, which is critical under tools such as
     * valgrind that intercept execve and may not handle signals safely
     * during their exec interception phase.
     */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read > 0 || (n_read < 0 && errno == EINTR));
    close(sync_read_fd);
    if (n_read < 0) {
        pid_t wait_result;
        perror("read exec-sync");
        kill(child_pid, SIGKILL);
        do {
            wait_result = waitpid(child_pid, NULL, 0);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == -1) {
            perror("waitpid");
        }
        return -1;
    }
    return 0;
}

/**
 * @brief Wait for the child process to exit and collect its exit status
 * @return The child's exit status if successfully reaped, EXIT_FAILURE
 *         otherwise
 *
 * Polls for the child's termination. Once the quit signal has been
 * forwarded, a 5-second SIGKILL timeout is applied if the child does not
 * exit in time. Translates signal termination to shell-compatible exit
 * codes (128 + signal number).
 *
 * Handles the race where a quit signal is delivered after
 * run_command_mode()'s is_quit_flag_set() check: if quit_flag becomes set
 * during polling, the exact received signal is forwarded to the child
 * process group so it exits with the correct status (128 + signal number)
 * rather than waiting for the SIGKILL timeout.  A SIGCONT is always sent
 * first so that a stopped child (e.g. on macOS 10.7 where stopped processes
 * are invisible to the process iterator) is resumed before the forwarded
 * signal is delivered.
 *
 * The signal is delivered exactly once.  When run_command_mode() has
 * already forwarded it (the common case, since the quit flag is normally
 * set before this function is reached) this function must not send a
 * second one: two SIGINTs from a single Ctrl+C would cut short the
 * handler a program installs to shut itself down cleanly, and no shell
 * behaves that way.
 *
 * start_time is reset to the moment the signal is forwarded from inside
 * this function, giving the child the full CHILD_KILL_TIMEOUT_MS from the
 * point it first receives the forwarded signal (not from function entry).
 * No escalation is armed before that: a command that is still running
 * because limit_process() stopped watching it is waited for, not killed.
 *
 * @param child_pid PID of the child process to wait for
 * @param cfg Pointer to configuration structure (used for verbose output)
 * @param signal_forwarded Non-zero if the caller already forwarded the
 *                         quit signal to the child process group
 */
static int collect_child_exit_status(pid_t child_pid,
                                     const struct cpulimit_cfg *cfg,
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
    limit_process(child_pid, cfg->cpu_limit, cfg->include_children,
                  cfg->verbose);

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
            limit_process(found_pid, cfg->cpu_limit, cfg->include_children,
                          cfg->verbose);

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
