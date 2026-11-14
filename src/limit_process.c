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
#include <sys/param.h>

/**
 * @def WORK_RATIO_EPSILON
 * @brief Small value keeping work_ratio away from zero in divisions.
 */
#define WORK_RATIO_EPSILON 1e-12

/**
 * @def BASE_TIME_SLOT_US
 * @brief Base control time slot in microseconds.
 */
#define BASE_TIME_SLOT_US 100000

/**
 * @def STATS_SAMPLE_PERIOD
 * @brief Control cycles between verbose statistics lines.
 */
#define STATS_SAMPLE_PERIOD 10

/**
 * @def STATS_HEADER_PERIOD
 * @brief Control cycles between verbose header lines.
 */
#define STATS_HEADER_PERIOD 200

#if (STATS_HEADER_PERIOD % STATS_SAMPLE_PERIOD) != 0
#error "STATS_HEADER_PERIOD must be a multiple of STATS_SAMPLE_PERIOD"
#endif

/**
 * @def MIN_TIME_SLOT_US
 * @brief Lower bound for the dynamic time slot.
 */
#define MIN_TIME_SLOT_US BASE_TIME_SLOT_US

/**
 * @def MAX_TIME_SLOT_US
 * @brief Upper bound for the dynamic time slot.
 */
#define MAX_TIME_SLOT_US (BASE_TIME_SLOT_US * 5)

/**
 * @struct dynamic_time_slot_ctx
 * @brief Mutable state of the dynamic time-slot algorithm.
 */
struct dynamic_time_slot_ctx {
    /**
     * @brief Current smoothed slot in microseconds.
     */
    double time_slot;
    /**
     * @brief Non-zero after the first call seeded the state.
     */
    int initialized;
    /**
     * @brief Timestamp of the last load-based adjustment.
     */
    struct timespec last_update;
};

/**
 * @brief Adapt the control time slot to system load.
 *
 * @param ctx Algorithm state.
 * @return Time slot in microseconds.
 *
 * Scales the current slot by load / N_CPU / 0.3, clamps and EMA-smooths it,
 * then adds jitter to avoid tick synchronization. A failed load read is
 * retried on the next cycle.
 */
static double get_dynamic_time_slot(struct dynamic_time_slot_ctx *ctx) {
    struct timespec now;
    double load;
    /* getloadavg() is BSD/GNU, so the project wrapper needs _GNU_SOURCE. */

    if (!ctx->initialized) {
        ctx->initialized = 1;
        if (get_current_time(&ctx->last_update) == 0) {
            srandom((unsigned int)((unsigned long)ctx->last_update.tv_nsec ^
                                   (unsigned long)ctx->last_update.tv_sec));
        }
    } else if (get_current_time(&now) == 0 &&
               timediff_in_ms(&now, &ctx->last_update) >= 1000.0 &&
               cpulimit_getloadavg(&load, 1) == 1) {
        double new_time_slot;

        ctx->last_update = now;

        /* Widen the slot as load rises to throttle less often. */
        new_time_slot = ctx->time_slot * load / get_ncpu() / 0.3;
        new_time_slot =
            CLAMP(new_time_slot, MIN_TIME_SLOT_US, MAX_TIME_SLOT_US);

        /* Smooth the adaptation with a 0.6/0.4 EMA. */
        ctx->time_slot = ctx->time_slot * 0.6 + new_time_slot * 0.4;
    }

    /* Add small jitter to avoid synchronizing with timer ticks. */
    return ctx->time_slot * (0.95 + (double)(random() % 1001) / 10000.0);
}

/**
 * @brief Enforce a CPU usage limit on a process or process set.
 *
 * @param pid PID of the target.
 * @param cpu_limit Limit in core equivalents, in (0, N_CPU].
 * @param include_children Non-zero to include descendants.
 * @param verbose Non-zero to print statistics and per-attempt diagnostics.
 * @param prior_scan_failures Consecutive scan failures already recorded, so
 *        the scan diagnostic prints once per streak.
 * @return One of the LIMIT_PROCESS_* codes; every non-OK value means the
 *         target is no longer limited.
 *
 * @note Blocks until the target terminates or is_quit_flag_set() is true,
 *       and resumes suspended members before returning where possible.
 */

int limit_process(pid_t pid, double cpu_limit, int include_children,
                  int verbose, unsigned int prior_scan_failures) {
    struct process_set proc_set;
    struct dynamic_time_slot_ctx time_slot_ctx = {BASE_TIME_SLOT_US, 0, {0, 0}};
    int cycle_counter = 0, ncpu = get_ncpu();
    int resume_failed;
    int scan_failed = 0;
    int target_replaced = 0;
    double work_ratio;
    int is_stopped = 0;

    cpu_limit = CLAMP(cpu_limit, WORK_RATIO_EPSILON, ncpu);
    work_ratio = cpu_limit / ncpu;

    /* Raise cpulimit's own priority so it stays responsive under load. */
    increase_priority();

    if (init_process_set(&proc_set, pid, include_children) != 0) {
        fprintf(stderr, "Failed to initialize process group for PID %ld\n",
                (long)pid);
        /* Exiting here would orphan the command child. */
        return LIMIT_PROCESS_ERROR;
    }

    if (verbose) {
        printf("Process group of PID %ld: %lu member(s)\n",
               (long)proc_set.target_pid,
               (unsigned long)process_set_member_count(&proc_set));
    }

    /* An explicitly named PID that never joined the group is not limitable. */
    /* Distinguish it from a target that was limited and then exited. */
    if (proc_set.target_pid > 0 && process_set_member_count(&proc_set) == 0) {
        /* Release the group: this early return bypasses the loop cleanup. */
        close_process_set(&proc_set);
        return LIMIT_PROCESS_NO_TARGET;
    }

    /* Alternate work and sleep quanta to hold the target CPU usage. */
    while (!is_quit_flag_set()) {
        double cpu_usage, work_time_ns, sleep_time_ns, time_slot, slot_time_ns;
        struct timespec work_time, sleep_time;
        int scan_ret;

        scan_ret = update_process_set(&proc_set);

        /* A replaced target must not read as a successful empty group. */
        if (proc_set.target_replaced) {
            target_replaced = 1;
            break;
        }

        if (scan_ret != 0) {
            /* Report a failed scan once per streak, and only with -v. */
            if (prior_scan_failures == 0 && verbose) {
                fprintf(
                    stderr,
                    "Process group scan failed; CPU limiting stopped for PID %ld, the target is no longer limited\n",
                    (long)pid);
            }
            scan_failed = 1;
            break;
        }

        if (process_set_is_empty(&proc_set)) {
            if (verbose) {
                printf("No running target process found.\n");
            }
            break;
        }

        cpu_usage = get_process_set_cpu_usage(&proc_set);

        /* Adjust work_ratio by the deviation from the target usage. */
        /* Leave it unchanged until a real CPU measurement exists. */
        if (cpu_usage >= 0) {
            /* Cap the per-cycle gain to keep the correction bounded. */
            double gain = cpu_limit / MAX(cpu_usage, WORK_RATIO_EPSILON);
            gain = CLAMP(gain, 0.5, 2.0);
            work_ratio = CLAMP(work_ratio * gain, WORK_RATIO_EPSILON,
                               1 - WORK_RATIO_EPSILON);
        }

        time_slot = get_dynamic_time_slot(&time_slot_ctx);

        slot_time_ns = time_slot * 1000.0;
        work_time_ns = slot_time_ns * work_ratio;
        /* Keep both quanta at or above 1 ns so neither phase is skipped. */
        if (work_time_ns > slot_time_ns - 1.0) {
            work_time_ns = slot_time_ns - 1.0;
        }
        if (work_time_ns < 1.0) {
            work_time_ns = 1.0;
        }
        nsec_to_timespec(work_time_ns, &work_time);

        sleep_time_ns = slot_time_ns - work_time_ns;
        if (sleep_time_ns < 1.0) {
            sleep_time_ns = 1.0;
        }
        nsec_to_timespec(sleep_time_ns, &sleep_time);

        if (verbose && cycle_counter % STATS_SAMPLE_PERIOD == 0) {
            if (cycle_counter % STATS_HEADER_PERIOD == 0) {
                printf("\n%9s%16s%16s%14s\n", "%CPU", "work quantum",
                       "sleep quantum", "active rate");
            }
            if (cpu_usage >= 0) {
                printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n", cpu_usage * 100,
                       work_time_ns / 1000, sleep_time_ns / 1000,
                       work_ratio * 100);
            } else {
                printf("%9s%13.0f us%13.0f us%13.2f%%\n", "n/a",
                       work_time_ns / 1000, sleep_time_ns / 1000,
                       work_ratio * 100);
            }
        }

        /* WORK PHASE: allow the group to run. */
        if (work_time.tv_sec > 0 || work_time.tv_nsec > 0) {
            if (is_stopped) {
                process_set_send_signal(&proc_set, SIGCONT, verbose);
                is_stopped = 0;
                /* An empty group ends this attempt. */
                if (process_set_is_empty(&proc_set)) {
                    break;
                }
            }
            sleep_timespec(&work_time);
        }

        if (is_quit_flag_set()) {
            break;
        }

        /* SLEEP PHASE: suspend the group. */
        if (sleep_time.tv_sec > 0 || sleep_time.tv_nsec > 0) {
            if (!is_stopped) {
                process_set_send_signal(&proc_set, SIGSTOP, verbose);
                is_stopped = 1;
                /* An empty group ends this attempt. */
                if (process_set_is_empty(&proc_set)) {
                    break;
                }
            }
            sleep_timespec(&sleep_time);
        }

        if (is_quit_flag_set()) {
            break;
        }

        cycle_counter = (cycle_counter + 1) % STATS_HEADER_PERIOD;
    }

    /* Resume every suspended member before returning. */
    resume_failed = process_set_send_signal(&proc_set, SIGCONT, verbose);

    close_process_set(&proc_set);

    if (resume_failed > 0) {
        /* A member that could not be resumed needs a manual kill -CONT. */
        fprintf(
            stderr,
            "Warning: %d process(es) left suspended at shutdown; run 'kill -CONT <pid>' for each to recover.\n",
            resume_failed);
        /* The limit was applied, so this is not LIMIT_PROCESS_ERROR. */
        /* Stranded PIDs outrank a replaced target in the return code. */
        if (target_replaced) {
            fprintf(
                stderr,
                "The target PID %ld now belongs to a different process, which was never limited.\n",
                (long)pid);
        }
        if (scan_failed) {
            return LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED;
        }
        return LIMIT_PROCESS_STRANDED;
    }

    /* Everything was resumed, but limiting stopped early. */
    if (scan_failed) {
        return LIMIT_PROCESS_SCAN_FAILED;
    }

    /* A replaced target must not be reported as LIMIT_PROCESS_OK. */
    if (target_replaced) {
        return LIMIT_PROCESS_NO_TARGET;
    }

    return LIMIT_PROCESS_OK;
}
