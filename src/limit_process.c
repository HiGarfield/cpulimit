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

#include "limit_process.h"

#include "list.h"
#include "process_group.h"
#include "process_iterator.h"
#include "process_table.h"
#include "signal_handler.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

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
 * @brief Dynamically calculate the time slot based on system load
 * @return The calculated dynamic time slot in microseconds
 * @note This function adapts the time slot based on system load to
 *       optimize responsiveness under varying system conditions.
 */
static double get_dynamic_time_slot(void) {
    static double time_slot = TIME_SLOT;
    static const double MIN_TIME_SLOT =
                            TIME_SLOT, /* Minimum allowed time slot */
        MAX_TIME_SLOT = TIME_SLOT * 5; /* Maximum allowed time slot */
    static struct timespec last_update = {0, 0};
    struct timespec now;
    double load, new_time_slot;

    /* Skip updates if the last check was less than 1000 ms ago */
    if (get_current_time(&now) == 0 &&
        timediff_in_ms(&now, &last_update) < 1000.0) {
        return time_slot;
    }

    /* Get the system load average */
    if (getloadavg(&load, 1) != 1) {
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
 * @brief Send a signal to all processes in a process group
 * @param procgroup Pointer to the process group structure
 * @param sig Signal to send to each process
 * @param verbose Verbose output flag
 */
static void send_signal_to_processes(struct process_group *procgroup, int sig,
                                     int verbose) {
    struct list_node *node = first_node(procgroup->proclist);
    while (node != NULL) {
        struct list_node *next_node = node->next;
        pid_t pid = ((struct process *)node->data)->pid;
        if (kill(pid, sig) != 0) {
            if (verbose && errno != ESRCH) {
                fprintf(stderr, "Failed to send signal %d to PID %ld: %s\n",
                        sig, (long)pid, strerror(errno));
            }
            delete_node(procgroup->proclist, node);
            process_table_del(procgroup->proctable, pid);
        }
        node = next_node;
    }
}

/**
 * @brief Apply CPU usage limiting to a process or process group
 * @param pid Process ID of the target process
 * @param limit CPU usage limit (0.0 to N_CPU)
 * @param include_children Flag to include child processes
 * @param verbose Verbose output flag
 * @note This function implements the main CPU limiting algorithm that
 *       alternates between letting the target process run and stopping
 *       it to enforce the CPU usage limit.
 */
void limit_process(pid_t pid, double limit, int include_children, int verbose) {
    /* Process group to manage */
    struct process_group proc_group;
    /* Cycle counter for periodic verbose output */
    int cycle_counter = 0;
    /* Work ratio: fraction of time slot the process should run */
    double work_ratio = limit / get_ncpu();
    /* Flag indicating if the process group is currently suspended */
    int is_suspended = 0;

    /* Increase priority of the current process to reduce overhead */
    increase_priority();

    /* Initialize the process group (including children if needed) */
    if (init_process_group(&proc_group, pid, include_children) != 0) {
        fprintf(stderr, "Failed to initialize process group for PID %ld\n",
                (long)pid);
        exit(EXIT_FAILURE);
    }

    if (verbose) {
        printf("Process group of PID %ld: %lu member(s)\n",
               (long)proc_group.target_pid,
               (unsigned long)get_list_count(proc_group.proclist));
    }

    /* Main control loop until quit_flag is set */
    while (!is_quit_flag_set()) {
        double cpu_usage, work_time_ns, sleep_time_ns, time_slot;
        struct timespec work_time, sleep_time;

        /* Update process group status and remove terminated processes */
        update_process_group(&proc_group);

        /* Exit if no more target processes are running */
        if (is_empty_list(proc_group.proclist)) {
            if (verbose) {
                printf("No running target process found.\n");
            }
            break;
        }

        /* Estimate CPU usage of all processes in the group */
        cpu_usage = get_process_group_cpu_usage(&proc_group);
        /* If CPU usage cannot be estimated, set it to the limit */
        cpu_usage = cpu_usage < 0 ? limit : cpu_usage;

        /* Adjust work ratio based on current CPU usage vs. limit */
        work_ratio = work_ratio * limit / MAX(cpu_usage, EPSILON);
        /* Clamp work ratio to valid range (0, 1) */
        work_ratio = CLAMP(work_ratio, EPSILON, 1 - EPSILON);

        /* Get the dynamic time slot duration */
        time_slot = get_dynamic_time_slot();

        /* Calculate work and sleep times in nanoseconds */
        work_time_ns = time_slot * 1000 * work_ratio;
        nsec2timespec(work_time_ns, &work_time);

        sleep_time_ns = time_slot * 1000 - work_time_ns;
        nsec2timespec(sleep_time_ns, &sleep_time);

        if (verbose) {
            /* Print CPU usage statistics every 10 cycles */
            if (cycle_counter % 10 == 0) {
                if (cycle_counter % 200 == 0) {
                    printf("\n%9s%16s%16s%14s\n", "%CPU", "work quantum",
                           "sleep quantum", "active rate");
                }
                printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n", cpu_usage * 100,
                       work_time_ns / 1000, sleep_time_ns / 1000,
                       work_ratio * 100);
            }
        }

        if (work_time.tv_nsec > 0 || work_time.tv_sec > 0) {
            if (is_suspended) {
                /* Resume suspended processes */
                send_signal_to_processes(&proc_group, SIGCONT, verbose);
                is_suspended = 0;
            }
            /* Allow processes to run during work interval */
            sleep_timespec(&work_time);
        }

        if (is_quit_flag_set()) {
            break;
        }

        if (sleep_time.tv_nsec > 0 || sleep_time.tv_sec > 0) {
            if (!is_suspended) {
                /* Suspend processes during sleep interval */
                send_signal_to_processes(&proc_group, SIGSTOP, verbose);
                is_suspended = 1;
            }
            /* Wait during sleep interval */
            sleep_timespec(&sleep_time);
        }

        cycle_counter = (cycle_counter + 1) % 200;
    }

    /* If quit_flag is set, resume suspended processes before exiting */
    if (is_quit_flag_set() && is_suspended) {
        send_signal_to_processes(&proc_group, SIGCONT, 0);
    }

    /* Clean up process group resources */
    close_process_group(&proc_group);
}
