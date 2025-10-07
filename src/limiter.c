/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
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

void run_command_mode(const struct cpulimitcfg *cfg)
{
    pid_t cmd_runner_pid = fork();
    if (cmd_runner_pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if (cmd_runner_pid == 0)
    {
        /* Command runner process: execute the command */
        setpgid(0, 0);

        execvp(cfg->command_args[0], cfg->command_args);

        /* Following lines will only be reached if execvp fails */
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    else
    {
        int child_exit_status, found_cmd_runner;

        /* Limiter process: limit the command runner */
        if (cfg->verbose)
        {
            printf("Limiting process %ld\n", (long)cmd_runner_pid);
        }
        limit_process(cmd_runner_pid, cfg->limit, cfg->include_children,
                      cfg->verbose);
        if (is_quit_flag_set())
        {
            kill(-cmd_runner_pid, SIGTERM);
        }

        child_exit_status = EXIT_FAILURE;
        found_cmd_runner = 0;

        while (1)
        {
            int status;
            pid_t wpid = waitpid(-cmd_runner_pid, &status, 0);
            if (wpid < 0)
            {
                if (errno == EINTR)
                {
                    continue; /* Interrupted by a signal, retry wait */
                }
                if (errno != ECHILD)
                {
                    perror("waitpid");
                }
                break;
            }
            if (wpid == cmd_runner_pid)
            {
                found_cmd_runner = 1;
                if (WIFEXITED(status))
                {
                    child_exit_status = WEXITSTATUS(status);
                    if (cfg->verbose)
                    {
                        printf("Process %ld exited with status %d\n",
                               (long)cmd_runner_pid, child_exit_status);
                    }
                }
                else if (WIFSIGNALED(status))
                {
                    int signal_number = WTERMSIG(status);
                    child_exit_status = 128 + signal_number;
                    if (cfg->verbose)
                    {
                        printf("Process %ld terminated by signal %d\n",
                               (long)cmd_runner_pid, signal_number);
                    }
                }
                else
                {
                    printf("Process %ld terminated abnormally\n",
                           (long)cmd_runner_pid);
                    child_exit_status = EXIT_FAILURE;
                }
            }
        }
        exit(found_cmd_runner ? child_exit_status : EXIT_FAILURE);
    }
}

void run_pid_or_exe_mode(const struct cpulimitcfg *cfg)
{
    /* Set waiting time between process searches */
    const struct timespec wait_time = {2, 0};
    int pid_mode = cfg->target_pid > 0;
    int exe_mode = cfg->exe_name != NULL;
    while (!is_quit_flag_set())
    {
        pid_t found_pid = 0;
        if (pid_mode)
        {
            /* Search for the process by PID */
            found_pid = find_process_by_pid(cfg->target_pid);
            if (found_pid <= 0)
            {
                printf("No process found or you aren't allowed to control it\n");
            }
        }
        else if (exe_mode)
        {
            /* Search for the process by name */
            found_pid = find_process_by_name(cfg->exe_name);
            if (found_pid == 0)
            {
                printf("No process found\n");
            }
            else if (found_pid < 0)
            {
                printf("Process found but you aren't allowed to control it\n");
            }
        }

        if (found_pid > 0)
        {
            if (found_pid == getpid())
            {
                printf("Target process %ld is cpulimit itself! Aborting\n",
                       (long)found_pid);
                exit(EXIT_FAILURE);
            }
            printf("Process %ld found\n", (long)found_pid);
            limit_process(found_pid, cfg->limit, cfg->include_children, cfg->verbose);
        }

        if (cfg->lazy_mode || is_quit_flag_set())
        {
            break;
        }

        sleep_timespec(&wait_time);
    }
    exit(EXIT_SUCCESS);
}
