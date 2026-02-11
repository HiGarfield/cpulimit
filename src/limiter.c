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
 * @brief Run the program in command execution mode
 * @param cfg Pointer to the configuration structure
 * @note This function forks a child process to execute a command and then
 *       limits the CPU usage of that command and its children.
 */
void run_command_mode(const struct cpulimitcfg *cfg) {
    pid_t cmd_runner_pid; /* PID of child process that runs the command */
    int sync_pipe[2];     /* Pipe for parent-child sync: [0]=read, [1]=write*/

    /* Create a pipe for synchronization between parent and child */
    if (pipe(sync_pipe) < 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    /* Fork a child process to run the command */
    cmd_runner_pid = fork();
    if (cmd_runner_pid < 0) {
        perror("fork");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        exit(EXIT_FAILURE);
    } else if (cmd_runner_pid == 0) {
        /* ========== CHILD PROCESS: COMMAND RUNNER ========== */
        /* This code runs in the child process that executes the user command */

        /*
         * Create a new process group with the child as leader.
         * This allows us to control all related processes together.
         */
        if (setpgid(0, 0) < 0) {
            perror("setpgid");
            close(sync_pipe[0]);
            close(sync_pipe[1]);
            exit(EXIT_FAILURE);
        }

        /* Close read end of pipe (child only writes to parent) */
        close(sync_pipe[0]);
        /* Send sync signal to parent: single byte 'A' indicates child ready */
        if (write(sync_pipe[1], "A", 1) != 1) {
            perror("write sync");
        }
        /* Close write end after sending signal */
        close(sync_pipe[1]);

        /*
         * Replace child process with user's command.
         * execvp searches PATH for executable in cfg->command_args[0].
         * Only returns on error.
         */
        execvp(cfg->command_args[0], cfg->command_args);

        /* execvp failed - only reached if execvp() returns (error case) */
        perror("execvp");
        exit(EXIT_FAILURE);
    } else {
        /* ========== PARENT PROCESS: CPU LIMITER CONTROLLER ========== */
        /* This code runs in parent process that monitors and limits child */
        int child_exit_status = EXIT_FAILURE; /* Default exit status */
        char ack;                   /* Buffer for sync byte from child */
        char found_cmd_runner = 0;  /* Flag: 1 if child reaped successfully*/
        struct timespec start_time; /* Timestamp for timeout calculation */

        /* Close write end of pipe (parent only reads from child) */
        close(sync_pipe[1]);
        /* Wait for child to be ready: read the synchronization byte */
        if (read(sync_pipe[0], &ack, 1) != 1 || ack != 'A') {
            perror("read sync");
            close(sync_pipe[0]);
            /* Clean up the child process that may be running */
            waitpid(cmd_runner_pid, NULL, 0);
            exit(EXIT_FAILURE);
        }
        close(sync_pipe[0]);

        /*
         * Apply CPU usage limits to child process and optionally its children.
         * limit_process() runs in separate monitoring thread/process.
         */
        if (cfg->verbose) {
            printf("Limiting process %ld\n", (long)cmd_runner_pid);
        }
        limit_process(cmd_runner_pid, cfg->limit, cfg->include_children,
                      cfg->verbose);

        /*
         * Check if quit flag was set (e.g., via signal handler).
         * If so, gracefully terminate entire process group.
         */
        if (is_quit_flag_set()) {
            /* Send SIGTERM to entire process group (negative PID) */
            kill(-cmd_runner_pid, SIGTERM);
        }

        /* Record start time for timeout monitoring */
        get_current_time(&start_time);

        /*
         * Monitoring loop: wait for child process(es) to terminate.
         * We wait for any process in child's process group (-cmd_runner_pid).
         * WNOHANG prevents blocking if processes are still running.
         */
        while (1) {
            int status; /* Child exit status */
            /* Wait for any process in child's process group without blocking */
            pid_t wpid = waitpid(-cmd_runner_pid, &status, WNOHANG);

            if (wpid == cmd_runner_pid) {
                /* Main child process we spawned has terminated */
                found_cmd_runner = 1; /* Mark that we found and reaped child */

                if (WIFEXITED(status)) {
                    /* Child exited normally with an exit code */
                    child_exit_status = WEXITSTATUS(status);
                    if (cfg->verbose) {
                        printf("Process %ld exited with status %d\n",
                               (long)cmd_runner_pid, child_exit_status);
                    }
                } else if (WIFSIGNALED(status)) {
                    /* Child was terminated by a signal */
                    int signal_number = WTERMSIG(status);
                    /* Convention: exit code = 128 + signal number */
                    child_exit_status = 128 + signal_number;
                    if (cfg->verbose) {
                        printf("Process %ld terminated by signal %d\n",
                               (long)cmd_runner_pid, signal_number);
                    }
                } else {
                    /* Child terminated abnormally (not by exit or signal) */
                    printf("Process %ld terminated abnormally\n",
                           (long)cmd_runner_pid);
                    child_exit_status = EXIT_FAILURE;
                }

            } else if (wpid == 0) {
                /*
                 * No child changed state yet (WNOHANG returned 0).
                 * Check if timeout exceeded.
                 */
                const struct timespec sleep_time = {0, 50000000L}; /* 50ms */
                struct timespec current_time;
                get_current_time(&current_time);

                /* 5s timeout: if more than 5000ms passed since start */
                if (timediff_in_ms(&current_time, &start_time) > 5000.0) {
                    if (cfg->verbose) {
                        printf("Process %ld timed out, sending SIGKILL\n",
                               (long)cmd_runner_pid);
                    }
                    /* Forcefully kill entire process group after timeout */
                    kill(-cmd_runner_pid, SIGKILL);
                }
                /* Sleep briefly to avoid busy-waiting, then check again */
                sleep_timespec(&sleep_time);

            } else if (wpid < 0) {
                /* waitpid returned an error */
                if (errno == EINTR) {
                    /* waitpid interrupted by signal, try again */
                    continue;
                }
                if (errno != ECHILD) {
                    /* Real error (not "no child processes") */
                    perror("waitpid");
                }
                /* No more child processes or unrecoverable error */
                break;
            }
        }

        /*
         * Exit with child's exit status if successfully reaped,
         * otherwise exit with failure status
         */
        exit(found_cmd_runner ? child_exit_status : EXIT_FAILURE);
    }
}

/**
 * @brief Run the program in PID or executable name mode
 * @param cfg Pointer to the configuration structure
 * @note This function continuously searches for the target process (by PID or
 *       executable name) and applies CPU limiting when found.
 */
void run_pid_or_exe_mode(const struct cpulimitcfg *cfg) {
    /* Set waiting time between process searches */
    const struct timespec wait_time = {2, 0};
    int pid_mode = cfg->target_pid > 0;

    while (!is_quit_flag_set()) {
        pid_t found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                                   : find_process_by_name(cfg->exe_name);

        if (found_pid == 0) {
            printf("Process cannot be found\n");
        } else if (found_pid < 0) {
            printf("No permission to control process\n");
            break;
        } else {
            /* Prevent limiting cpulimit itself */
            if (found_pid == getpid()) {
                printf("Target process %ld is cpulimit itself! Aborting\n",
                       (long)found_pid);
                exit(EXIT_FAILURE);
            }
            printf("Process %ld found\n", (long)found_pid);
            /* Apply CPU limiting to the found process */
            limit_process(found_pid, cfg->limit, cfg->include_children,
                          cfg->verbose);
        }

        /* Exit if in lazy mode or quit flag is set */
        if (cfg->lazy_mode || is_quit_flag_set()) {
            break;
        }

        /* Wait before searching again */
        sleep_timespec(&wait_time);
    }
    exit(EXIT_SUCCESS);
}
