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
#include <unistd.h>

/* Very small value to prevent division by zero in calculations */
#ifndef EPSILON
#define EPSILON 1e-12
#endif

/*
 * Base control time slot in microseconds.
 * Each limiting cycle divides this slot into work time and sleep time.
 * The dynamic algorithm may adjust this value based on system load.
 */
#define TIME_SLOT 100000

/**
 * @brief Calculate dynamic time slot duration based on system load
 * @return Time slot duration in microseconds
 *
 * This function adapts the control time slot to system conditions:
 * - Under low load: uses smaller time slots for precise control
 * - Under high load: uses larger time slots to reduce overhead
 *
 * The algorithm:
 * 1. Maintains a static time slot that evolves over time
 * 2. Reads system load average via getloadavg()
 * 3. Adjusts time slot proportionally to load per CPU
 * 4. Applies smoothing (exponential moving average) to avoid oscillation
 * 5. Adds small randomization to prevent synchronization with system tick
 *
 * Updates at most once per second to avoid excessive system calls.
 *
 * @note This function is not thread-safe and must only be called from a
 *       single thread.
 */
static double get_dynamic_time_slot(void) {
    static double time_slot = TIME_SLOT;
    static const double MIN_TIME_SLOT =
                            TIME_SLOT, /* Minimum: 100ms for precision */
        MAX_TIME_SLOT = TIME_SLOT * 5; /* Maximum: 500ms to reduce overhead */
    static struct timespec last_update = {0, 0};
    struct timespec now;
    double load, new_time_slot;

    /* First call: initialize random seed and timestamp */
    if (last_update.tv_sec == 0 && last_update.tv_nsec == 0) {
        if (get_current_time(&last_update) == 0) {
            /* Seed PRNG with current time for randomization */
            srandom((unsigned int)(last_update.tv_nsec ^ last_update.tv_sec));
            goto out;
        }
    }

    /* Skip update if time retrieval fails */
    if (get_current_time(&now) != 0) {
        goto out;
    }

    /* Update at most once per second */
    if (timediff_in_ms(&now, &last_update) < 1000.0) {
        goto out;
    }

    /* Get 1-minute load average */
    if (getloadavg(&load, 1) != 1) {
        goto out;
    }

    last_update = now;

    /*
     * Calculate new time slot based on load:
     * - load / ncpu = normalized load per CPU
     * - Divide by 0.3 to scale: target is 30% baseline load
     * - Higher load → larger time slot → less frequent adjustments
     */
    new_time_slot = time_slot * load / get_ncpu() / 0.3;
    new_time_slot = CLAMP(new_time_slot, MIN_TIME_SLOT, MAX_TIME_SLOT);

    /*
     * Smooth adaptation using exponential moving average:
     * new_value = 0.6 * old_value + 0.4 * measured_value
     * This prevents rapid oscillation in time slot size.
     */
    time_slot = time_slot * 0.6 + new_time_slot * 0.4;

out:
    /*
     * Add approximately -5% to +5% random jitter to prevent synchronization
     * with system timer ticks. This improves accuracy by avoiding systematic
     * bias.
     */
    return time_slot * (0.95 + (double)(random() % 1000) / 10000.0);
}

/**
 * @brief Send a signal to all processes in a process group
 * @param procgroup Pointer to process group structure containing target
 *                  processes
 * @param sig Signal number to send (e.g., SIGSTOP, SIGCONT)
 * @param verbose If non-zero, print errors when signal delivery fails
 *
 * Iterates through all processes in the group and sends the specified signal.
 * If signal delivery fails (e.g., process terminated), the process is removed
 * from the group to avoid repeated errors.
 *
 * @note Safe iteration: stores next node before potential deletion
 */
static void send_signal_to_processes(struct process_group *procgroup, int sig,
                                     int verbose) {
    struct list_node *node = first_node(procgroup->proclist);
    while (node != NULL) {
        struct list_node *next_node =
            node->next; /* Save before potential deletion */
        pid_t pid = ((struct process *)node->data)->pid;
        if (kill(pid, sig) != 0) {
            /*
             * Signal delivery failed. Common reasons:
             * - ESRCH: Process no longer exists
             * - EPERM: Permission denied (rare in this context)
             */
            if (verbose && errno != ESRCH) {
                fprintf(stderr, "Failed to send signal %d to PID %ld: %s\n",
                        sig, (long)pid, strerror(errno));
            }
            /* Remove dead/inaccessible process from tracking */
            delete_node(procgroup->proclist, node);
            process_table_del(procgroup->proctable, pid);
        }
        node = next_node;
    }
}

/**
 * @brief Enforce CPU usage limit on a process or process group
 * @param pid Process ID of the target process to limit
 * @param limit CPU usage limit expressed in CPU cores (core equivalents),
 *              in the range (0, N_CPU]. Example: on a 4-core system, limit=0.5
 *              means 50% of one core (12.5% of total capacity), and limit=2.0
 *              means two full cores (50% of total capacity).
 * @param include_children If non-zero, limit applies to target and all
 *                         descendants; if zero, limit applies only to target
 *                         process
 * @param verbose If non-zero, print periodic statistics about CPU usage and
 *                control; if zero, operate silently
 *
 * This function implements the core CPU limiting algorithm using
 * SIGSTOP/SIGCONT:
 * 1. Monitors the process group's actual CPU usage
 * 2. Calculates appropriate work/sleep intervals to achieve the target limit
 * 3. Alternately sends SIGCONT (allow execution) and SIGSTOP (suspend
 * execution)
 * 4. Dynamically adjusts timing based on measured CPU usage
 * 5. Continues until the target terminates or quit signal received
 *
 * @note This function blocks until target terminates or is_quit_flag_set()
 *       returns true
 * @note Always resumes suspended processes (sends SIGCONT) before returning
 */
void limit_process(pid_t pid, double limit, int include_children, int verbose) {
    struct process_group proc_group;
    int cycle_counter = 0, ncpu = get_ncpu();
    double work_ratio; /* Fraction of time processes should be running */
    int stopped =
        0; /* Current state: 1 if processes are stopped, 0 if running */

    /* Clamp limit to valid range and calculate initial work ratio */
    limit = CLAMP(limit, EPSILON, ncpu);
    work_ratio = limit / ncpu;

    /*
     * Increase priority of cpulimit itself to ensure it can
     * respond quickly to enforce limits even under high system load.
     */
    increase_priority();

    /* Initialize process group tracking structure */
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

    /*
     * Main control loop: alternate between allowing execution and suspending
     * processes to maintain target CPU usage.
     */
    while (!is_quit_flag_set()) {
        double cpu_usage, work_time_ns, sleep_time_ns, time_slot;
        struct timespec work_time, sleep_time;

        /* Refresh process list and update CPU usage measurements */
        update_process_group(&proc_group);

        /* Exit if all target processes have terminated */
        if (is_empty_list(proc_group.proclist)) {
            if (verbose) {
                printf("No running target process found.\n");
            }
            break;
        }

        /* Get current CPU usage of all processes in group */
        cpu_usage = get_process_group_cpu_usage(&proc_group);
        /*
         * If CPU usage unknown (first samples), assume maximum.
         * This prevents over-execution during initialization.
         */
        cpu_usage = cpu_usage < 0 ? ncpu : cpu_usage;

        /*
         * Adaptive control: adjust work ratio based on deviation from target.
         * If actual usage > limit: decrease work_ratio (more stopping)
         * If actual usage < limit: increase work_ratio (less stopping)
         * Formula: new_ratio = old_ratio * (target / actual)
         */
        work_ratio = work_ratio * limit / MAX(cpu_usage, EPSILON);
        /* Ensure work_ratio stays in valid range, never exactly 0 or 1 */
        work_ratio = CLAMP(work_ratio, EPSILON, 1 - EPSILON);

        /* Get time slot duration (may vary based on system load) */
        time_slot = get_dynamic_time_slot();

        /* Split time slot into work and sleep periods */
        work_time_ns = time_slot * 1000 * work_ratio;
        nsec2timespec(work_time_ns, &work_time);

        sleep_time_ns = time_slot * 1000 - work_time_ns;
        nsec2timespec(sleep_time_ns, &sleep_time);

        if (verbose) {
            /* Display statistics every 10 cycles, header every 200 cycles */
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

        /*
         * WORK PHASE: Allow processes to execute
         */
        if (work_time.tv_sec > 0 || work_time.tv_nsec > 0) {
            if (stopped) {
                /* Resume all stopped processes */
                send_signal_to_processes(&proc_group, SIGCONT, verbose);
                stopped = 0;
                /* Recheck process list after signaling */
                if (is_empty_list(proc_group.proclist)) {
                    break;
                }
            }
            /* Allow processes to run for work_time duration */
            sleep_timespec(&work_time);
        }

        /* Check for termination request before sleep phase */
        if (is_quit_flag_set()) {
            break;
        }

        /*
         * SLEEP PHASE: Suspend processes to limit CPU usage
         */
        if (sleep_time.tv_sec > 0 || sleep_time.tv_nsec > 0) {
            if (!stopped) {
                /* Stop all running processes */
                send_signal_to_processes(&proc_group, SIGSTOP, verbose);
                stopped = 1;
                /* Recheck process list after signaling */
                if (is_empty_list(proc_group.proclist)) {
                    break;
                }
            }
            /* Keep processes suspended for sleep_time duration */
            sleep_timespec(&sleep_time);
        }

        /* Increment cycle counter with wraparound */
        cycle_counter = (cycle_counter + 1) % 200;
    }

    /*
     * If terminated from terminal (Ctrl+C) and both stdin/stdout are TTY,
     * print newline for clean terminal output.
     */
    if (is_quit_flag_set() && is_terminated_by_tty() && isatty(STDIN_FILENO) &&
        isatty(STDOUT_FILENO)) {
        fputc('\n', stdout);
        fflush(stdout);
    }

    /*
     * Critical: Always resume any stopped processes before exit.
     * Leaving processes in stopped state would render them unusable.
     */
    send_signal_to_processes(&proc_group, SIGCONT, 0);

    /* Release process tracking resources */
    close_process_group(&proc_group);
}
