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

#include "cpu_count.h"
#include "process_set.h"
#include "signal_handler.h"
#include "time_util.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * The test harness renames getloadavg() to cpulimit_test_getloadavg() via a
 * -D flag applied to the sources compiled into the test binary. On libcs that
 * provide a getloadavg() prototype (glibc, uClibc >= 1.0.42) that rename also
 * rewrites the prototype, so no extra declaration is needed. On uClibc < 1.0.42
 * (flagged by CPULIMIT_IMPL_GETLOADAVG in util.h) there is no prototype to
 * rewrite, so declare the renamed symbol here to avoid an implicit-declaration
 * warning.
 */
#ifdef CPULIMIT_IMPL_GETLOADAVG
int cpulimit_test_getloadavg(double *loadavg, int nelem);
#endif

/**
 * @def WORK_RATIO_EPSILON
 * @brief Very small positive value used to prevent division by zero and
 *        bound work_ratio strictly away from 0 and 1
 *
 * Used in:
 * - MAX(cpu_usage, WORK_RATIO_EPSILON): prevents division by zero in
 *   work_ratio calculation
 * - CLAMP(work_ratio, WORK_RATIO_EPSILON, 1 - WORK_RATIO_EPSILON): ensures
 *   both work and sleep phases always have positive duration
 */
#define WORK_RATIO_EPSILON 1e-12

/**
 * @def BASE_TIME_SLOT_US
 * @brief Base control time slot in microseconds
 *
 * Each limiting cycle divides this slot into work time and sleep time.
 * Used as both the initial value and the minimum bound for the dynamic
 * time slot maintained in get_dynamic_time_slot().
 */
#define BASE_TIME_SLOT_US 100000

/**
 * @def STATS_SAMPLE_PERIOD
 * @brief Number of control cycles between each verbose statistics line
 *
 * In verbose mode, CPU usage and control parameters are printed every
 * STATS_SAMPLE_PERIOD cycles.  Keeping this relatively low (10) provides
 * timely feedback without flooding the terminal.
 */
#define STATS_SAMPLE_PERIOD 10

/**
 * @def STATS_HEADER_PERIOD
 * @brief Number of control cycles between verbose statistics header lines
 *
 * A column-header line is printed every STATS_HEADER_PERIOD cycles so that
 * the output remains readable when scrolling.  Must be a multiple of
 * STATS_SAMPLE_PERIOD.
 */
#define STATS_HEADER_PERIOD 200

#if (STATS_HEADER_PERIOD % STATS_SAMPLE_PERIOD) != 0
#error "STATS_HEADER_PERIOD must be a multiple of STATS_SAMPLE_PERIOD"
#endif

/**
 * @def MIN_TIME_SLOT_US
 * @brief Minimum dynamic time slot in microseconds
 *
 * Derived from BASE_TIME_SLOT_US; kept as a macro because it is a compile
 * time constant, not mutable state.
 */
#define MIN_TIME_SLOT_US BASE_TIME_SLOT_US

/**
 * @def MAX_TIME_SLOT_US
 * @brief Maximum dynamic time slot in microseconds
 *
 * Derived from BASE_TIME_SLOT_US; kept as a macro because it is a compile
 * time constant, not mutable state.
 */
#define MAX_TIME_SLOT_US (BASE_TIME_SLOT_US * 5)

/**
 * @struct dynamic_time_slot_ctx
 * @brief Explicit state for the dynamic time-slot algorithm
 *
 * Holds the mutable state that was previously kept in static local
 * variables inside get_dynamic_time_slot().  The caller owns an instance
 * of this structure and passes it to get_dynamic_time_slot().
 */
struct dynamic_time_slot_ctx {
    /** Current smoothed time slot in microseconds. */
    double time_slot;
    /** Non-zero after the first call has seeded the timestamp and PRNG. */
    int initialized;
    /** Timestamp of the most recent load-based adjustment. */
    struct timespec last_update;
};

/**
 * @brief Calculate dynamic time slot duration based on system load
 * @param ctx Pointer to dynamic_time_slot_ctx holding the algorithm state
 * @return Time slot duration in microseconds
 *
 * This function adapts the control time slot to system conditions:
 * - Under low load: uses smaller time slots for precise control
 * - Under high load: uses larger time slots to reduce overhead
 *
 * The algorithm:
 * 1. Maintains a time slot that evolves over time (stored in ctx)
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
static double get_dynamic_time_slot(struct dynamic_time_slot_ctx *ctx) {
    struct timespec now;
    double load;

    /* First call: initialize timestamp and seed PRNG for jitter */
    if (!ctx->initialized) {
        ctx->initialized = 1;
        if (get_current_time(&ctx->last_update) == 0) {
            /* Seed PRNG with current time for randomization */
            srandom((unsigned int)((unsigned long)ctx->last_update.tv_nsec ^
                                   (unsigned long)ctx->last_update.tv_sec));
        }
    } else if (get_current_time(&now) == 0 &&
               timediff_in_ms(&now, &ctx->last_update) >= 1000.0 &&
               getloadavg(&load, 1) == 1) {
        double new_time_slot;

        ctx->last_update = now;

        /*
         * Calculate new time slot based on load:
         * - load / ncpu = normalized load per CPU
         * - Divide by 0.3 to scale: target is 30% baseline load
         * - Higher load -> larger time slot -> less frequent
         *   adjustments.
         */
        new_time_slot = ctx->time_slot * load / get_ncpu() / 0.3;
        new_time_slot =
            CLAMP(new_time_slot, MIN_TIME_SLOT_US, MAX_TIME_SLOT_US);

        /*
         * Smooth adaptation using exponential moving average:
         * new_value = 0.6 * old_value + 0.4 * measured_value
         * This prevents rapid oscillation in time slot size.
         */
        ctx->time_slot = ctx->time_slot * 0.6 + new_time_slot * 0.4;
    }

    /*
     * Add approximately -5% to +5% random jitter to prevent synchronization
     * with system timer ticks. This improves accuracy by avoiding systematic
     * bias.
     */
    return ctx->time_slot * (0.95 + (double)(random() % 1001) / 10000.0);
}

/**
 * @brief Enforce CPU usage limit on a process or process set
 * @param pid Process ID of the target process to limit
 * @param cpu_limit CPU usage limit expressed in CPU cores (core
 *              equivalents), in the range (0, N_CPU]. Example: on a 4-core
 *              system, cpu_limit=0.5 means 50% of one core (12.5% of total
 *              capacity), and cpu_limit=2.0 means two full cores (50% of
 *              total capacity).
 * @param include_children If non-zero, limit applies to target and all
 *                         descendants; if zero, limit applies only to target
 *                         process
 * @param verbose If non-zero, print periodic statistics about CPU usage and
 *                control; if zero, operate silently
 *
 * This function implements the core CPU limiting algorithm using
 * SIGSTOP/SIGCONT:
 * 1. Monitors the process set's actual CPU usage
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
int limit_process(pid_t pid, double cpu_limit, int include_children,
                  int verbose) {
    struct process_set proc_set;
    struct dynamic_time_slot_ctx time_slot_ctx = {BASE_TIME_SLOT_US, 0, {0, 0}};
    int cycle_counter = 0, ncpu = get_ncpu();
    /* Fraction of time processes should be running */
    double work_ratio;
    /* Current state: 1 if processes are stopped, 0 if running */
    int is_stopped = 0;

    /* Clamp cpu_limit to valid range and calculate initial work ratio */
    cpu_limit = CLAMP(cpu_limit, WORK_RATIO_EPSILON, ncpu);
    work_ratio = cpu_limit / ncpu;

    /*
     * Increase priority of cpulimit itself to ensure it can
     * respond quickly to enforce limits even under high system load.
     */
    increase_priority();

    /* Initialize process set tracking structure */
    if (init_process_set(&proc_set, pid, include_children) != 0) {
        fprintf(stderr, "Failed to initialize process group for PID %ld\n",
                (long)pid);
        /*
         * Report the failure to the caller instead of terminating the
         * process.  Exiting here abandoned a command-mode child that had
         * already been forked: it kept running at full speed, nobody
         * waited for it and cpulimit reported EXIT_FAILURE without ever
         * seeing the command's own status.  Nothing has been stopped at
         * this point, so there is nothing to resume either.
         */
        return LIMIT_PROCESS_ERROR;
    }

    if (verbose) {
        printf("Process group of PID %ld: %lu member(s)\n",
               (long)proc_set.target_pid,
               (unsigned long)process_set_member_count(&proc_set));
    }

    /*
     * Main control loop: alternate between allowing execution and suspending
     * processes to maintain target CPU usage.
     */
    while (!is_quit_flag_set()) {
        double cpu_usage, work_time_ns, sleep_time_ns, time_slot, slot_time_ns;
        struct timespec work_time, sleep_time;

        /* Refresh process list and update CPU usage measurements */
        if (update_process_set(&proc_set) != 0) {
            break;
        }

        /* Exit if all target processes have terminated */
        if (process_set_is_empty(&proc_set)) {
            if (verbose) {
                printf("No running target process found.\n");
            }
            break;
        }

        /* Get current CPU usage of all processes in group */
        cpu_usage = get_process_set_cpu_usage(&proc_set);

        /*
         * Adaptive control: adjust work ratio based on deviation from target.
         * If actual usage > cpu_limit: decrease work_ratio (more stopping)
         * If actual usage < cpu_limit: increase work_ratio (less stopping)
         * Formula: new_ratio = old_ratio * (target / actual).
         *
         * A negative cpu_usage means "not measured yet": the first cycles
         * have no CPU-time delta to compare against. Substituting a guess
         * here used to scale work_ratio by cpu_limit/ncpu on the very
         * first cycle, so a run asking for 50% of one core started at a
         * small fraction of that before converging. Leave the ratio
         * untouched until there is a real measurement to act on.
         */
        if (cpu_usage >= 0) {
            work_ratio =
                work_ratio * cpu_limit / MAX(cpu_usage, WORK_RATIO_EPSILON);
            /*
             * Ensure work_ratio stays in valid range, never exactly 0 or 1
             */
            work_ratio = CLAMP(work_ratio, WORK_RATIO_EPSILON,
                               1 - WORK_RATIO_EPSILON);
        }

        /* Get time slot duration (may vary based on system load) */
        time_slot = get_dynamic_time_slot(&time_slot_ctx);

        /* Split time slot into work and sleep periods */
        slot_time_ns = time_slot * 1000.0;
        work_time_ns = slot_time_ns * work_ratio;
        /*
         * Keep both quanta at or above one nanosecond. nsec_to_timespec()
         * truncates, so anything below 1 ns becomes a zero timespec and the
         * whole phase below is skipped -- and with it the SIGCONT or
         * SIGSTOP that phase is responsible for sending. A target stopped
         * during the sleep phase would then never be resumed by its own
         * work phase. Clamping keeps the signal sequence intact even for
         * extreme limits, and leaves ordinary values untouched.
         */
        if (work_time_ns < 1.0) {
            work_time_ns = 1.0;
        }
        if (work_time_ns > slot_time_ns - 1.0) {
            work_time_ns = slot_time_ns - 1.0;
        }
        nsec_to_timespec(work_time_ns, &work_time);

        sleep_time_ns = slot_time_ns - work_time_ns;
        if (sleep_time_ns < 1.0) {
            sleep_time_ns = 1.0;
        }
        nsec_to_timespec(sleep_time_ns, &sleep_time);

        if (verbose) {
            if (cycle_counter % STATS_SAMPLE_PERIOD == 0) {
                if (cycle_counter % STATS_HEADER_PERIOD == 0) {
                    printf("\n%9s%16s%16s%14s\n", "%CPU", "work quantum",
                           "sleep quantum", "active rate");
                }
                if (cpu_usage >= 0) {
                    printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n",
                           cpu_usage * 100, work_time_ns / 1000,
                           sleep_time_ns / 1000, work_ratio * 100);
                } else {
                    /*
                     * No measurement for this cycle yet; report that
                     * rather than a fabricated figure.
                     */
                    printf("%9s%13.0f us%13.0f us%13.2f%%\n", "n/a",
                           work_time_ns / 1000, sleep_time_ns / 1000,
                           work_ratio * 100);
                }
            }
        }

        /*
         * WORK PHASE: Allow processes to execute.
         */
        if (work_time.tv_sec > 0 || work_time.tv_nsec > 0) {
            if (is_stopped) {
                /* Resume all stopped processes */
                process_set_send_signal(&proc_set, SIGCONT, verbose);
                is_stopped = 0;
                /* Recheck process list after signaling */
                if (process_set_is_empty(&proc_set)) {
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
         * SLEEP PHASE: Suspend processes to limit CPU usage.
         */
        if (sleep_time.tv_sec > 0 || sleep_time.tv_nsec > 0) {
            if (!is_stopped) {
                /* Stop all running processes */
                process_set_send_signal(&proc_set, SIGSTOP, verbose);
                is_stopped = 1;
                /* Recheck process list after signaling */
                if (process_set_is_empty(&proc_set)) {
                    break;
                }
            }
            /* Keep processes suspended for sleep_time duration */
            sleep_timespec(&sleep_time);
        }

        /* Check for termination request after sleep phase */
        if (is_quit_flag_set()) {
            break;
        }

        /* Increment cycle counter with wraparound */
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
     * This also resumes processes that dropped out of the group while
     * suspended (see record_stopped_pid()).
     */
    process_set_send_signal(&proc_set, SIGCONT, 0);

    /* Release process tracking resources */
    close_process_set(&proc_set);

    return LIMIT_PROCESS_OK;
}
