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

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"
#include "list.h"
#include "process_group.h"
#include "process_iterator.h"
#include "util.h"

#ifndef EPSILON
/* Define a very small value to avoid division by zero */
#define EPSILON 1e-12
#endif

/*
 * Control time slot in microseconds
 * Each slot is split into a working slice and a sleeping slice
 */
#define TIME_SLOT 100000

/* Quit flag for handling SIGINT and SIGTERM signals */
static volatile sig_atomic_t quit_flag = 0;

/**
 * Signal handler for SIGINT and SIGTERM signals.
 * Sets the quit_flag to 1 when a termination signal is received.
 *
 * @param sig Signal number (SIGINT or SIGTERM).
 */
static void sig_handler(int sig)
{
    /* Handle SIGINT and SIGTERM signals by setting quit_flag to 1 */
    switch (sig)
    {
    case SIGINT:
    case SIGTERM:
        quit_flag = 1;
        break;
    default:
        break;
    }
}

/**
 * Dynamically calculates the time slot based on system load.
 * This allows the program to adapt to varying system conditions.
 *
 * @return The calculated dynamic time slot in microseconds.
 */
static double get_dynamic_time_slot(void)
{
    static double time_slot = TIME_SLOT;
    static const double MIN_TIME_SLOT = TIME_SLOT, /* Minimum allowed time slot */
        MAX_TIME_SLOT = TIME_SLOT * 5;             /* Maximum allowed time slot */
    static struct timespec last_update = {0, 0};
    struct timespec now;
    double load, new_time_slot;

    /* Skip updates if the last check was less than 1000 ms ago */
    if (get_current_time(&now) == 0 &&
        timediff_in_ms(&now, &last_update) < 1000.0)
    {
        return time_slot;
    }

    /* Get the system load average */
    if (getloadavg(&load, 1) != 1)
    {
        return time_slot;
    }

    last_update = now;

    /* Adjust the time slot based on system load and number of CPUs */
    new_time_slot = time_slot * load / get_ncpu() / 0.3;
    new_time_slot = MAX(new_time_slot, MIN_TIME_SLOT);
    new_time_slot = MIN(new_time_slot, MAX_TIME_SLOT);

    /* Smoothly adjust the time slot using a moving average */
    time_slot = time_slot * 0.6 + new_time_slot * 0.4;

    return time_slot;
}

/**
 * Sends a specified signal to all processes in a given process group.
 *
 * @param procgroup Pointer to the process group containing the list of
 *                  processes to which the signal will be sent.
 * @param sig The signal to be sent to each process (e.g., SIGCONT/SIGSTOP).
 * @param verbose Verbose mode flag.
 */
static void send_signal_to_processes(struct process_group *procgroup,
                                     int sig, int verbose)
{
    struct list_node *node = procgroup->proclist->first;
    while (node != NULL)
    {
        struct list_node *next_node = node->next;
        const struct process *proc = (const struct process *)node->data;
        if (kill(proc->pid, sig) != 0)
        {
            if (verbose)
            {
                fprintf(stderr, "Failed to send signal %d to PID %ld: %s\n",
                        sig, (long)proc->pid, strerror(errno));
            }
            delete_node(procgroup->proclist, node);
            remove_process(procgroup, proc->pid);
        }
        node = next_node;
    }
}

/**
 * Controls the CPU usage of a process (and optionally its children).
 * Limits the amount of time the process can run based on a given percentage.
 *
 * @param pid Process ID of the target process.
 * @param limit The CPU usage limit (0 to N_CPU).
 * @param include_children Whether to include child processes.
 * @param verbose Verbose mode flag.
 */
static void limit_process(pid_t pid, double limit, int include_children, int verbose)
{
    /* Process group */
    struct process_group pgroup;
    /* Slice of time in which the process is allowed to work */
    struct timespec twork;
    /* Slice of time in which the process is stopped */
    struct timespec tsleep;
    /* Counter to help with printing status */
    int c = 0;

    /* The ratio of the time the process is allowed to work (range 0 to 1) */
    double workingrate = -1;

    /* Increase priority of the current process to reduce overhead */
    increase_priority();

    /* Initialize the process group (including children if needed) */
    init_process_group(&pgroup, pid, include_children);

    if (verbose)
        printf("Members in the process group owned by %ld: %lu\n",
               (long)pgroup.target_pid,
               (unsigned long)get_list_count(pgroup.proclist));

    /* Main loop to control the process until quit_flag is set */
    while (!quit_flag)
    {
        /* CPU usage of the controlled processes */
        /* 1 means that the processes are using 100% cpu */
        double pcpu;
        double twork_total_nsec, tsleep_total_nsec;
        double time_slot;

        /* Update the process group, including checking for dead processes */
        update_process_group(&pgroup);

        /* Exit if no more processes are running */
        if (is_empty_list(pgroup.proclist))
        {
            if (verbose)
                printf("No more processes.\n");
            break;
        }

        /* Estimate CPU usage of all processes in the group */
        pcpu = get_process_group_cpu_usage(&pgroup);

        /* Adjust the work and sleep time slices based on CPU usage */
        if (pcpu < 0)
        {
            /* Initialize workingrate if it's the first cycle */
            pcpu = limit;
            workingrate = limit;
        }
        else
        {
            /* Adjust workingrate based on CPU usage and limit */
            workingrate = workingrate * limit / MAX(pcpu, EPSILON);
        }

        /* Clamp workingrate to the valid range (0, 1) */
        workingrate = MIN(workingrate, 1 - EPSILON);
        workingrate = MAX(workingrate, EPSILON);

        /* Get the dynamic time slot */
        time_slot = get_dynamic_time_slot();

        /* Calculate work and sleep times in nanoseconds */
        twork_total_nsec = time_slot * 1000 * workingrate;
        nsec2timespec(twork_total_nsec, &twork);

        tsleep_total_nsec = time_slot * 1000 - twork_total_nsec;
        nsec2timespec(tsleep_total_nsec, &tsleep);

        if (verbose)
        {
            /* Print CPU usage statistics every 10 cycles */
            if (c % 200 == 0)
                printf("\n%9s%16s%16s%14s\n",
                       "%CPU", "work quantum", "sleep quantum", "active rate");

            if (c % 10 == 0 && c > 0)
                printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n",
                       pcpu * 100, twork_total_nsec / 1000,
                       tsleep_total_nsec / 1000, workingrate * 100);
        }

        /* Resume processes in the group */
        send_signal_to_processes(&pgroup, SIGCONT, verbose);
        /* Allow processes to run during the work slice */
        sleep_timespec(&twork);

        if (tsleep.tv_nsec > 0 || tsleep.tv_sec > 0)
        {
            /* Stop processes during the sleep slice if needed */
            send_signal_to_processes(&pgroup, SIGSTOP, verbose);
            /* Allow the processes to sleep during the sleep slice */
            sleep_timespec(&tsleep);
        }
        c = (c + 1) % 200;
    }

    /* If the quit_flag is set, resume all processes before exiting */
    if (quit_flag)
    {
        send_signal_to_processes(&pgroup, SIGCONT, 0);
    }

    /* Clean up the process group */
    close_process_group(&pgroup);
}

/**
 * Handles the cleanup when a termination signal is received.
 * Clears the current line on the console if the quit flag is set.
 */
static void quit_handler(void)
{
    /* If quit_flag is set, clear the current line on console (fix for ^C issue) */
    if (quit_flag)
    {
        printf("\r");
    }
}

int main(int argc, char *argv[])
{
    struct cpulimitcfg cfg;
    /* Set waiting time between process searches */
    struct timespec wait_time = {2, 0};

    /* Signal action struct for handling interrupts */
    struct sigaction sa;

    /* Register the quit handler to run at program exit */
    if (atexit(quit_handler) != 0)
    {
        fprintf(stderr, "Failed to register quit_handler\n");
        exit(EXIT_FAILURE);
    }

    /* Parse command line arguments */
    parse_arguments(argc, argv, &cfg);

    /* Set up signal handlers for SIGINT and SIGTERM */
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    sa.sa_handler = &sig_handler;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) != 0)
    {
        perror("Failed to set SIGINT handler");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0)
    {
        perror("Failed to set SIGTERM handler");
        exit(EXIT_FAILURE);
    }

    /* Print number of CPUs if in verbose mode */
    if (cfg.verbose)
    {
        /* Get the number of CPUs available */
        int n_cpu = get_ncpu();
        if (n_cpu > 1)
        {
            printf("%d CPUs detected\n", n_cpu);
        }
        else if (n_cpu == 1)
        {
            printf("%d CPU detected\n", n_cpu);
        }
    }

    /* Handle command mode (run a command and limit its CPU usage) */
    if (cfg.command_mode)
    {
        pid_t child;

        /* If verbose, print the command being executed */
        if (cfg.verbose)
        {
            char *const *arg;
            printf("Running command: '%s", cfg.command_args[0]);
            for (arg = cfg.command_args + 1; *arg != NULL; arg++)
            {
                printf(" %s", *arg);
            }
            printf("'\n");
        }

        /* Fork a child process to run the command */
        child = fork();
        if (child < 0)
        {
            exit(EXIT_FAILURE);
        }
        else if (child == 0)
        {
            /* Execute the command in the child process */
            int ret = execvp(cfg.command_args[0], cfg.command_args);
            perror("execvp"); /* Display error if execvp fails */
            exit(ret);
        }
        else
        {
            /* Parent process forks another limiter process to control CPU usage */
            pid_t limiter;
            limiter = fork();
            if (limiter < 0)
            {
                perror("fork");
                exit(EXIT_FAILURE);
            }
            else if (limiter > 0)
            {
                /* Wait for both child and limiter processes to complete */
                int status_process;
                int status_limiter;
                waitpid(child, &status_process, 0);
                waitpid(limiter, &status_limiter, 0);
                if (WIFEXITED(status_process))
                {
                    if (cfg.verbose)
                        printf("Process %ld terminated with exit status %d\n",
                               (long)child, WEXITSTATUS(status_process));
                    exit(WEXITSTATUS(status_process));
                }
                printf("Process %ld terminated abnormally\n", (long)child);
                exit(status_process);
            }
            else
            {
                /* Limiter process controls the CPU usage of the child process */
                if (cfg.verbose)
                    printf("Limiting process %ld\n", (long)child);
                limit_process(child, cfg.limit, cfg.include_children, cfg.verbose);
                exit(EXIT_SUCCESS);
            }
        }
    }

    /* Monitor and limit the target process specified by PID or executable name */
    while (!quit_flag)
    {
        pid_t ret = 0;
        if (cfg.target_pid > 0)
        {
            /* Search for the process by PID */
            ret = find_process_by_pid(cfg.target_pid);
            if (ret <= 0)
            {
                printf("No process found or you aren't allowed to control it\n");
            }
        }
        else
        {
            /* Search for the process by executable name */
            ret = find_process_by_name(cfg.exe_name);
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
                cfg.target_pid = ret;
            }
        }

        /* If a process is found, start limiting its CPU usage */
        if (ret > 0)
        {
            if (ret == getpid())
            {
                printf("Target process %ld is cpulimit itself! Aborting because it makes no sense\n",
                       (long)ret);
                exit(EXIT_FAILURE);
            }
            printf("Process %ld found\n", (long)cfg.target_pid);
            limit_process(cfg.target_pid, cfg.limit, cfg.include_children, cfg.verbose);
        }

        /* Break the loop if lazy mode is enabled or quit flag is set */
        if (cfg.lazy_mode || quit_flag)
            break;

        /* Wait for 2 seconds before the next process search */
        sleep_timespec(&wait_time);
    }

    return 0;
}
