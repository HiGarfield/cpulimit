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

#include "cli.h"
#include "limit_process.h"
#include "process_group.h"
#include "signal_handler.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Execute and monitor a user-specified command with CPU limiting
 * @param cfg Pointer to configuration structure containing command and options
 *
 * This function implements command execution mode (-COMMAND):
 * 1. Forks a child process to execute the specified command
 * 2. Creates a new process group for the child
 * 3. Applies CPU limiting to the command and optionally its descendants
 * 4. Waits for command completion and returns its exit status
 *
 * The parent process monitors the child and handles:
 * - Normal exit (returns child's exit code)
 * - Signal termination (returns 128 + signal number)
 * - Timeout after termination request (sends SIGKILL)
 *
 * @note This function calls exit() and does not return
 */
void run_command_mode(const struct cpulimitcfg *cfg) {
    pid_t
        cmd_runner_pid; /* PID of forked child that will execute the command */
    int sync_pipe[2];   /* Pipe for parent-child synchronization */

    /* Create pipe for synchronization: parent waits for child setup completion
     */
    if (pipe(sync_pipe) < 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    /* Fork to create child process that will execute user command */
    cmd_runner_pid = fork();
    if (cmd_runner_pid < 0) {
        perror("fork");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        exit(EXIT_FAILURE);
    } else if (cmd_runner_pid == 0) {
        /*
         * This block executes in the child process.
         * The child will become the command specified by the user.
         */

        /*
         * Create new process group with child as leader.
         * This allows limiting the entire process tree (child + descendants)
         * and enables sending signals to all related processes via -PGID.
         */
        if (setpgid(0, 0) < 0) {
            perror("setpgid");
            close(sync_pipe[0]);
            close(sync_pipe[1]);
            _exit(EXIT_FAILURE);
        }

        /* Close unused read end of pipe */
        close(sync_pipe[0]);
        /*
         * Signal parent that child initialization is complete.
         * Parent blocks until receiving this byte.
         */
        if (write(sync_pipe[1], "A", 1) != 1) {
            perror("write sync");
            close(sync_pipe[1]);
            _exit(EXIT_FAILURE);
        }
        close(sync_pipe[1]);

        /*
         * Replace child process image with the user command.
         * execvp() searches PATH for the executable and transfers control.
         * If successful, this function never returns.
         */
        execvp(cfg->command_args[0], cfg->command_args);

        /* Execution reaches here only if execvp() failed */
        perror("execvp");
        _exit(EXIT_FAILURE);
    } else {
        /*
         * This block executes in the parent process.
         * Parent is responsible for:
         * 1. Waiting for child initialization
         * 2. Applying CPU limiting to child
         * 3. Monitoring child termination
         * 4. Cleaning up and returning child's exit status
         */
        int child_exit_status =
            EXIT_FAILURE;           /* Default if child not properly reaped */
        char ack;                   /* Synchronization byte from child */
        char found_cmd_runner = 0;  /* 1 if successfully reaped child PID */
        struct timespec start_time; /* Timestamp when termination starts */
        ssize_t n_read;             /* Bytes read from pipe */

        /* Close unused write end of pipe */
        close(sync_pipe[1]);
        /*
         * Block until child signals readiness by writing to pipe.
         * This ensures child has completed setpgid() before parent continues.
         */
        do {
            n_read = read(sync_pipe[0], &ack, 1);
        } while (n_read < 0 && errno == EINTR);
        if (n_read != 1 || ack != 'A') {
            perror("read sync");
            close(sync_pipe[0]);
            /* Reap child to prevent zombie */
            waitpid(cmd_runner_pid, NULL, 0);
            exit(EXIT_FAILURE);
        }
        close(sync_pipe[0]);

        /*
         * Apply CPU limiting to child process.
         * If include_children is set, limit_process() also tracks and limits
         * all descendant processes. This call blocks until child terminates
         * or a quit signal is received.
         */
        if (cfg->verbose) {
            printf("Limiting process %ld\n", (long)cmd_runner_pid);
        }
        limit_process(cmd_runner_pid, cfg->limit, cfg->include_children,
                      cfg->verbose);

        /*
         * Check if user requested termination via signal (Ctrl+C, SIGTERM,
         * etc). If so, gracefully terminate the entire process group.
         */
        if (is_quit_flag_set()) {
            /*
             * Send SIGTERM to all processes in child's process group.
             * Negative PID targets the process group: -PGID
             */
            kill(-cmd_runner_pid, SIGTERM);
        }

        /* Record time for timeout monitoring during cleanup */
        if (get_current_time(&start_time) != 0) {
            exit(EXIT_FAILURE);
        }

        /*
         * Cleanup loop: reap all child processes whose process group matches
         * the command's process group. This waits only on our own children;
         * descendants further down the tree are reparented (typically to init)
         * when their direct parent exits and are reaped by that ancestor.
         */
        while (1) {
            int status;
            /*
             * Wait for any process in child's group without blocking.
             * WNOHANG allows checking timeout and other conditions.
             */
            pid_t wpid = waitpid(-cmd_runner_pid, &status, WNOHANG);

            if (wpid == cmd_runner_pid) {
                /* Primary child process has terminated */
                found_cmd_runner = 1;

                if (WIFEXITED(status)) {
                    /* Child exited normally via exit() or return from main() */
                    child_exit_status = WEXITSTATUS(status);
                    if (cfg->verbose) {
                        printf("Process %ld exited with status %d\n",
                               (long)cmd_runner_pid, child_exit_status);
                    }
                } else if (WIFSIGNALED(status)) {
                    /* Child was terminated by a signal (SIGTERM, SIGKILL, etc)
                     */
                    int signal_number = WTERMSIG(status);
                    /*
                     * Shell convention: exit status = 128 + signal number
                     * Example: SIGTERM (15) -> exit status 143
                     */
                    child_exit_status = 128 + signal_number;
                    if (cfg->verbose) {
                        printf("Process %ld terminated by signal %d\n",
                               (long)cmd_runner_pid, signal_number);
                    }
                } else {
                    /* Abnormal termination (neither exit nor signal) */
                    printf("Process %ld terminated abnormally\n",
                           (long)cmd_runner_pid);
                    child_exit_status = EXIT_FAILURE;
                }

            } else if (wpid == 0) {
                /*
                 * No state changes yet (WNOHANG returned immediately).
                 * Check if we've exceeded timeout for graceful termination.
                 */
                const struct timespec sleep_time = {
                    0, 50000000L}; /* 50 milliseconds */
                struct timespec current_time;
                if (get_current_time(&current_time) != 0) {
                    exit(EXIT_FAILURE);
                }

                /*
                 * After 5 seconds, forcefully kill any remaining processes.
                 * This handles cases where processes ignore SIGTERM.
                 */
                if (timediff_in_ms(&current_time, &start_time) > 5000.0) {
                    if (cfg->verbose) {
                        printf("Process %ld timed out, sending SIGKILL\n",
                               (long)cmd_runner_pid);
                    }
                    /* SIGKILL cannot be caught or ignored */
                    kill(-cmd_runner_pid, SIGKILL);
                }
                /* Brief sleep to avoid busy-waiting */
                sleep_timespec(&sleep_time);

            } else if (wpid < 0) {
                /* waitpid() encountered an error */
                if (errno == EINTR) {
                    /* Interrupted by signal, retry immediately */
                    continue;
                }
                if (errno != ECHILD) {
                    /* Real error (not just "no children") */
                    perror("waitpid");
                }
                /* ECHILD means no more children, exit loop */
                break;
            }
        }

        /*
         * Exit with child's exit status if we successfully reaped it,
         * otherwise exit with failure status.
         */
        exit(found_cmd_runner ? child_exit_status : EXIT_FAILURE);
    }
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
 * @note This function calls exit() and does not return
 */
void run_pid_or_exe_mode(const struct cpulimitcfg *cfg) {
    /* Wait interval between search attempts when target not found */
    const struct timespec wait_time = {2, 0}; /* 2 seconds */
    int pid_mode = cfg->target_pid > 0;
    int exit_status = EXIT_SUCCESS;

    while (!is_quit_flag_set()) {
        pid_t found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                                   : find_process_by_name(cfg->exe_name);

        if (found_pid == 0) {
            /* Process does not exist */
            printf("Process cannot be found\n");
            if (cfg->lazy_mode) {
                /* In lazy mode, missing target is an error condition. */
                exit_status = EXIT_FAILURE;
            }
        } else if (found_pid < 0) {
            /*
             * Process exists but cannot be controlled (permission denied).
             * Negative PID indicates EPERM error. No point retrying.
             */
            printf("No permission to control process\n");
            exit_status = EXIT_FAILURE;
            break;
        } else {
            /*
             * Sanity check: prevent cpulimit from limiting itself.
             * This could cause system instability or deadlock.
             */
            if (found_pid == getpid()) {
                printf("Target process %ld is cpulimit itself! Aborting\n",
                       (long)found_pid);
                exit(EXIT_FAILURE);
            }
            printf("Process %ld found\n", (long)found_pid);
            /*
             * Apply CPU limiting to the target process.
             * This call blocks until the process terminates or quit flag is
             * set.
             */
            limit_process(found_pid, cfg->limit, cfg->include_children,
                          cfg->verbose);
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
    exit(exit_status);
}
