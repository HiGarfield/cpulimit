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
    pid_t cmd_runner_pid;
    int sync_pipe[2];

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
        /* Command runner process: execute the command */
        if (setpgid(0, 0) < 0) {
            perror("setpgid");
            close(sync_pipe[0]);
            close(sync_pipe[1]);
            exit(EXIT_FAILURE);
        }

        close(sync_pipe[0]);
        /* Send synchronization signal to parent */
        if (write(sync_pipe[1], "A", 1) != 1) {
            perror("write sync");
        }
        close(sync_pipe[1]);

        execvp(cfg->command_args[0], cfg->command_args);

        /* Following lines will only be reached if execvp fails */
        perror("execvp");
        exit(EXIT_FAILURE);
    } else {
        int child_exit_status, found_cmd_runner;
        char ack;

        close(sync_pipe[1]);
        /* Wait for child to be ready before starting CPU limiting */
        if (read(sync_pipe[0], &ack, 1) != 1 || ack != 'A') {
            perror("read sync");
            close(sync_pipe[0]);
            waitpid(cmd_runner_pid, NULL, 0);
            exit(EXIT_FAILURE);
        }
        close(sync_pipe[0]);

        /* Limiter process: limit the CPU usage of the command runner */
        if (cfg->verbose) {
            printf("Limiting process %ld\n", (long)cmd_runner_pid);
        }
        limit_process(cmd_runner_pid, cfg->limit, cfg->include_children,
                      cfg->verbose);

        /* If quit flag is set, terminate the command runner and its group */
        if (is_quit_flag_set()) {
            kill(-cmd_runner_pid, SIGTERM);
        }

        child_exit_status = EXIT_FAILURE;
        found_cmd_runner = 0;

        /* Wait for all child processes in the command runner's process group */
        while (1) {
            int status;
            pid_t wpid = waitpid(-cmd_runner_pid, &status, 0);
            if (wpid < 0) {
                if (errno == EINTR) {
                    continue; /* Interrupted by a signal, retry wait */
                }
                if (errno != ECHILD) {
                    perror("waitpid");
                }
                break;
            }
            if (wpid == cmd_runner_pid) {
                found_cmd_runner = 1;
                if (WIFEXITED(status)) {
                    child_exit_status = WEXITSTATUS(status);
                    if (cfg->verbose) {
                        printf("Process %ld exited with status %d\n",
                               (long)cmd_runner_pid, child_exit_status);
                    }
                } else if (WIFSIGNALED(status)) {
                    int signal_number = WTERMSIG(status);
                    child_exit_status = 128 + signal_number;
                    if (cfg->verbose) {
                        printf("Process %ld terminated by signal %d\n",
                               (long)cmd_runner_pid, signal_number);
                    }
                } else {
                    printf("Process %ld terminated abnormally\n",
                           (long)cmd_runner_pid);
                    child_exit_status = EXIT_FAILURE;
                }
            }
        }
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
