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

/*
 * Very small positive value used to:
 * - Prevent division by zero in work_ratio calculation (MAX(cpu_usage,
 * WORK_RATIO_EPSILON))
 * - Bound work_ratio strictly away from 0 and 1 in
 *   CLAMP(work_ratio, WORK_RATIO_EPSILON, 1 - WORK_RATIO_EPSILON), ensuring
 *   both work and sleep phases always have positive duration
 */
#define WORK_RATIO_EPSILON 1e-12

/*
 * Base control time slot in microseconds.
 * Each limiting cycle divides this slot into work time and sleep time.
 * The dynamic algorithm may adjust this value based on system load.
 */
#define BASE_TIME_SLOT_US 100000

/*
 * Maximum control time slot in microseconds.
 * Limits how large the adaptive time slot can grow under high load.
 */
#define MAX_TIME_SLOT_US (BASE_TIME_SLOT_US * 5)

/*
 * Print a statistics line every STATS_SAMPLE_PERIOD control cycles
 * when verbose mode is active.
 */
#define STATS_SAMPLE_PERIOD 10

/*
 * Print the statistics column header every STATS_HEADER_PERIOD control
 * cycles (must be a multiple of STATS_SAMPLE_PERIOD).
 */
#define STATS_HEADER_PERIOD 200

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
    /* Initialized to BASE_TIME_SLOT_US; clamped to MAX_TIME_SLOT_US */
    static double time_slot = BASE_TIME_SLOT_US;
    static int initialized = 0;
    static struct timespec last_update = {0, 0};
    struct timespec now;
    double load;

    /* First call: initialize timestamp and seed PRNG for jitter */
    if (!initialized) {
        initialized = 1;
        if (get_current_time(&last_update) == 0) {
            /* Seed PRNG with current time for randomization */
            srandom((unsigned int)((unsigned long)last_update.tv_nsec ^
                                   (unsigned long)last_update.tv_sec));
        }
    } else if (get_current_time(&now) == 0 &&
               timediff_in_ms(&now, &last_update) >= 1000.0 &&
               getloadavg(&load, 1) == 1) {
        double new_time_slot;

        last_update = now;

        /*
         * Calculate new time slot based on load:
         * - load / ncpu = normalized load per CPU
         * - Divide by 0.3 to scale: target is 30% baseline load
         * - Higher load -> larger time slot -> less frequent
         *   adjustments
         */
        new_time_slot = time_slot * load / get_ncpu() / 0.3;
        new_time_slot = CLAMP(new_time_slot, (double)BASE_TIME_SLOT_US,
                              (double)MAX_TIME_SLOT_US);

        /*
         * Smooth adaptation using exponential moving average:
         * new_value = 0.6 * old_value + 0.4 * measured_value
         * This prevents rapid oscillation in time slot size.
         */
        time_slot = time_slot * 0.6 + new_time_slot * 0.4;
    }

    /*
     * Add approximately -5% to +5% random jitter to prevent synchronization
     * with system timer ticks. This improves accuracy by avoiding systematic
     * bias.
     */
    return time_slot * (0.95 + (double)(random() % 1000) / 10000.0);
}

/**
 * @brief Send a signal to all processes in a process group
 * @param proc_group Pointer to process group structure containing target
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
static void send_signal_to_processes(struct process_group *proc_group, int sig,
                                     int verbose) {
    struct list_node *node = first_node(proc_group->proc_list);
    while (node != NULL) {
        struct list_node *next_node =
            node->next; /* Save before potential deletion */
        pid_t pid;
        int kill_result;
        if (node->data == NULL) {
            /* Defensive: skip and remove any NULL-data nodes */
            delete_node(proc_group->proc_list, node);
            node = next_node;
            continue;
        }
        pid = ((const struct process *)node->data)->pid;
        do {
            kill_result = kill(pid, sig);
        } while (kill_result != 0 && errno == EINTR);

        if (kill_result != 0) {
            /*
             * Signal delivery failed. Common reasons:
             * - ESRCH: Process no longer exists
             * - EPERM: Permission denied (rare in this context)
             *
             * EINTR is transient and retried above, so all failures here
             * indicate a process that can no longer be reliably controlled.
             * Save errno before any other calls that may clobber it.
             */
            int saved_errno = errno;
            if (verbose && saved_errno != ESRCH) {
                fprintf(stderr, "Failed to send signal %d to PID %ld: %s\n",
                        sig, (long)pid, strerror(saved_errno));
            }
            /* Remove dead/inaccessible process from tracking */
            delete_node(proc_group->proc_list, node);
            delete_from_process_table(proc_group->proc_table, pid);
        }
        node = next_node;
    }
}

/**
 * @brief Execute one control phase: transition process state and sleep.
 * @param proc_group      Pointer to the process group being controlled.
 * @param target_sig      Signal to send if a state transition is needed
 *                        (SIGCONT to resume, SIGSTOP to suspend).
 * @param target_stopped  Desired stopped state: 0 = running, 1 = stopped.
 * @param duration        Duration to sleep during this phase.
 * @param verbose         If non-zero, log signal errors.
 * @param is_stopped      In/out flag tracking whether processes are stopped.
 * @return 1 if the outer control loop should exit (process list empty or
 *         quit flag set after sleeping), 0 to continue.
 *
 * If @p duration is zero the phase is skipped entirely (returns 0).
 * If the current state differs from @p target_stopped, the appropriate
 * signal is sent to all tracked processes and the state flag updated.
 * After sleeping, returns 1 if is_quit_flag_set() is true.
 */
static int execute_control_phase(struct process_group *proc_group,
                                 int target_sig, int target_stopped,
                                 const struct timespec *duration, int verbose,
                                 int *is_stopped) {
    if (duration->tv_sec == 0 && duration->tv_nsec == 0) {
        return 0; /* Zero-duration phase: nothing to do */
    }
    if (*is_stopped != target_stopped) {
        /* Transition to target state */
        send_signal_to_processes(proc_group, target_sig, verbose);
        *is_stopped = target_stopped;
        /* Exit loop if all processes have terminated */
        if (is_empty_list(proc_group->proc_list)) {
            return 1;
        }
    }
    sleep_timespec(duration);
    /* Signal check after sleeping */
    return is_quit_flag_set();
}

/**
 * @brief Print a line of CPU limiting statistics in verbose mode.
 * @param cycle       Current cycle counter value.
 * @param cpu_usage   Measured CPU usage as a fraction of one core.
 * @param work_ns     Work phase duration in nanoseconds.
 * @param sleep_ns    Sleep phase duration in nanoseconds.
 * @param work_ratio  Current work ratio (fraction of time processes run).
 *
 * Prints a column header every STATS_HEADER_PERIOD cycles and a data row
 * every STATS_SAMPLE_PERIOD cycles. Does nothing for cycles that fall
 * outside the sampling interval.
 */
static void print_limiter_stats(int cycle, double cpu_usage, double work_ns,
                                double sleep_ns, double work_ratio) {
    if (cycle % STATS_SAMPLE_PERIOD != 0) {
        return;
    }
    if (cycle % STATS_HEADER_PERIOD == 0) {
        printf("\n%9s%16s%16s%14s\n", "%CPU", "work quantum", "sleep quantum",
               "active rate");
    }
    printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n", cpu_usage * 100,
           work_ns / 1000, sleep_ns / 1000, work_ratio * 100);
}

/**
 * @brief Enforce CPU usage limit on a process or process group
 * @param pid Process ID of the target process to limit
 * @param limit CPU usage limit expressed in CPU cores (core equivalents), in
 *              the range (0, N_CPU]. Example: on a 4-core system,
 *              limit=0.5 means 50% of one core (12.5% of total capacity),
 *              and limit=2.0 means two full cores (50% of total capacity).
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
    int is_stopped =
        0; /* Current state: 1 if processes are stopped, 0 if running */

    /* Clamp limit to valid range and calculate initial work ratio */
    limit = CLAMP(limit, WORK_RATIO_EPSILON, ncpu);
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
               (unsigned long)get_list_count(proc_group.proc_list));
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
        if (is_empty_list(proc_group.proc_list)) {
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
        work_ratio = work_ratio * limit / MAX(cpu_usage, WORK_RATIO_EPSILON);
        /* Ensure work_ratio stays in valid range, never exactly 0 or 1 */
        work_ratio =
            CLAMP(work_ratio, WORK_RATIO_EPSILON, 1 - WORK_RATIO_EPSILON);

        /* Get time slot duration (may vary based on system load) */
        time_slot = get_dynamic_time_slot();

        /* Split time slot into work and sleep periods */
        work_time_ns = time_slot * 1000 * work_ratio;
        nsec2timespec(work_time_ns, &work_time);

        sleep_time_ns = time_slot * 1000 - work_time_ns;
        nsec2timespec(sleep_time_ns, &sleep_time);

        if (verbose) {
            print_limiter_stats(cycle_counter, cpu_usage, work_time_ns,
                                sleep_time_ns, work_ratio);
        }

        /*
         * WORK PHASE: Allow processes to execute for work_time duration.
         * Resumes stopped processes with SIGCONT if needed.
         */
        if (execute_control_phase(&proc_group, SIGCONT, 0, &work_time, verbose,
                                  &is_stopped)) {
            break;
        }

        /*
         * SLEEP PHASE: Suspend processes for sleep_time duration.
         * Stops running processes with SIGSTOP if needed.
         */
        if (execute_control_phase(&proc_group, SIGSTOP, 1, &sleep_time, verbose,
                                  &is_stopped)) {
            break;
        }

        /* Increment cycle counter; reset after STATS_HEADER_PERIOD cycles */
        cycle_counter = (cycle_counter + 1) % STATS_HEADER_PERIOD;
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
