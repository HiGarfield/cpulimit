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

#include "limit_process.h"
#include "list.h"
#include "process_group.h"
#include "process_iterator.h"
#include "signal_handler.h"
#include "util.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Define a very small value to avoid division by zero */
#ifndef EPSILON
#define EPSILON 1e-12
#endif

/*
 * Control time slot in microseconds
 * Each slot is split into a working slice and a sleeping slice
 */
#define TIME_SLOT 100000

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
    new_time_slot = CLAMP(new_time_slot, MIN_TIME_SLOT, MAX_TIME_SLOT);

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
    struct list_node *node = first_node(procgroup->proclist);
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

void limit_process(pid_t pid, double limit, int include_children, int verbose)
{
    /* Process group */
    struct process_group pgroup;
    /* Counter for the number of cycles */
    int cycle_counter = 0;
    /* Working rate for the process group in a time slot */
    double workingrate = limit / get_ncpu();
    /* Flag to indicate if the process group is sleeping */
    int pg_sleeping = 0;

    /* Increase priority of the current process to reduce overhead */
    increase_priority();

    /* Initialize the process group (including children if needed) */
    if (init_process_group(&pgroup, pid, include_children) != 0)
    {
        fprintf(stderr, "Failed to initialize process group for PID %ld\n",
                (long)pid);
        exit(EXIT_FAILURE);
    }

    if (verbose)
    {
        printf("Members in the process group owned by %ld: %lu\n",
               (long)pgroup.target_pid,
               (unsigned long)get_list_count(pgroup.proclist));
    }

    /* Main loop to control the process until quit_flag is set */
    while (!is_quit_flag_set())
    {
        double cpu_usage, twork_nsec, tsleep_nsec, time_slot;
        struct timespec twork, tsleep;

        /* Update the process group, including checking for dead processes */
        update_process_group(&pgroup);

        /* Exit if no more processes are running */
        if (is_empty_list(pgroup.proclist))
        {
            if (verbose)
            {
                printf("No more processes.\n");
            }
            break;
        }

        /* Estimate CPU usage of all processes in the group */
        cpu_usage = get_process_group_cpu_usage(&pgroup);
        /* If CPU usage cannot be estimated, set it to the limit */
        cpu_usage = cpu_usage < 0 ? limit : cpu_usage;

        /* Adjust workingrate based on CPU usage and limit */
        workingrate = workingrate * limit / MAX(cpu_usage, EPSILON);
        /* Clamp workingrate to the valid range (0, 1) */
        workingrate = CLAMP(workingrate, EPSILON, 1 - EPSILON);

        /* Get the dynamic time slot */
        time_slot = get_dynamic_time_slot();

        /* Calculate work and sleep times in nanoseconds */
        twork_nsec = time_slot * 1000 * workingrate;
        nsec2timespec(twork_nsec, &twork);

        tsleep_nsec = time_slot * 1000 - twork_nsec;
        nsec2timespec(tsleep_nsec, &tsleep);

        if (verbose)
        {
            /* Print CPU usage statistics every 10 cycles */
            if (cycle_counter % 200 == 0)
            {
                printf("\n%9s%16s%16s%14s\n",
                       "%CPU", "work quantum", "sleep quantum", "active rate");
            }

            if (cycle_counter % 10 == 0 && cycle_counter > 0)
            {
                printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n",
                       cpu_usage * 100, twork_nsec / 1000,
                       tsleep_nsec / 1000, workingrate * 100);
            }
        }

        if (twork.tv_nsec > 0 || twork.tv_sec > 0)
        {
            if (pg_sleeping)
            {
                /* Resume processes in the group */
                send_signal_to_processes(&pgroup, SIGCONT, verbose);
                pg_sleeping = 0;
            }
            /* Allow processes to run during the work slice */
            sleep_timespec(&twork);
        }

        if (tsleep.tv_nsec > 0 || tsleep.tv_sec > 0)
        {
            if (!pg_sleeping)
            {
                /* Stop processes during the sleep slice if needed */
                send_signal_to_processes(&pgroup, SIGSTOP, verbose);
                pg_sleeping = 1;
            }
            /* Allow the processes to sleep during the sleep slice */
            sleep_timespec(&tsleep);
        }

        cycle_counter = (cycle_counter + 1) % 200;
    }

    /* If the quit_flag is set, resume all processes before exiting */
    if (is_quit_flag_set())
    {
        send_signal_to_processes(&pgroup, SIGCONT, 0);
    }

    /* Clean up the process group */
    close_process_group(&pgroup);
}
