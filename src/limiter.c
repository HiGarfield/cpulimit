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
#include "limit_process.h"
#include "process_group.h"
#include "signal_handler.h"
#include "util.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

void run_command_mode(struct cpulimitcfg *cfg)
{
    pid_t cmd_runner_pid = fork();
    if (cmd_runner_pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if (cmd_runner_pid == 0)
    {
        int ret = execvp(cfg->command_args[0], cfg->command_args);
        perror("execvp");
        exit(ret);
    }
    else
    {
        pid_t limiter_pid = fork();
        if (limiter_pid < 0)
        {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        else if (limiter_pid > 0)
        {
            int status, cmd_runner_status = -1;
            while (1)
            {
                pid_t wpid = wait(&status);
                if (wpid > 0)
                {
                    if (wpid == cmd_runner_pid)
                    {
                        cmd_runner_status = status;
                    }
                }
                else if (wpid == -1)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            if (cmd_runner_status < 0)
            {
                printf("Error waiting for child process\n");
                exit(EXIT_FAILURE);
            }
            else if (WIFEXITED(cmd_runner_status))
            {
                int child_exit_status = WEXITSTATUS(cmd_runner_status);
                if (cfg->verbose)
                {
                    printf("Process %ld exited with status %d\n",
                           (long)cmd_runner_pid, child_exit_status);
                }
                exit(child_exit_status);
            }
            else if (WIFSIGNALED(cmd_runner_status))
            {
                int signal_number = WTERMSIG(cmd_runner_status);
                int child_exit_status = 128 + signal_number;
                if (cfg->verbose)
                {
                    printf("Process %ld terminated by signal %d\n",
                           (long)cmd_runner_pid, signal_number);
                }
                exit(child_exit_status);
            }
            else
            {
                printf("Process %ld terminated abnormally\n",
                       (long)cmd_runner_pid);
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            if (cfg->verbose)
            {
                printf("Limiting process %ld\n", (long)cmd_runner_pid);
            }
            limit_process(cmd_runner_pid, cfg->limit, cfg->include_children,
                          cfg->verbose);
            exit(EXIT_SUCCESS);
        }
    }
}

void run_normal_mode(struct cpulimitcfg *cfg)
{
    /* Set waiting time between process searches */
    const struct timespec wait_time = {2, 0};
    while (!is_quit_flag_set())
    {
        pid_t ret = 0;
        if (cfg->target_pid > 0)
        {
            /* If target_pid is set, search for the process by PID */
            ret = find_process_by_pid(cfg->target_pid);
            if (ret <= 0)
            {
                printf("No process found or you aren't allowed to control it\n");
            }
        }
        else
        {
            /* If target_pid is not set, search for the process by name */
            ret = find_process_by_name(cfg->exe_name);
            if (ret == 0)
            {
                printf("No process found\n");
            }
            else if (ret < 0)
            {
                printf("Process found but you aren't allowed to control it\n");
            }
            else
            {
                cfg->target_pid = ret;
            }
        }

        if (ret > 0)
        {
            if (ret == getpid())
            {
                printf("Target process %ld is cpulimit itself! Aborting\n",
                       (long)ret);
                exit(EXIT_FAILURE);
            }
            printf("Process %ld found\n", (long)cfg->target_pid);
            limit_process(cfg->target_pid, cfg->limit, cfg->include_children, cfg->verbose);
        }

        if (cfg->lazy_mode || is_quit_flag_set())
        {
            break;
        }

        sleep_timespec(&wait_time);
    }
    exit(EXIT_SUCCESS);
}
