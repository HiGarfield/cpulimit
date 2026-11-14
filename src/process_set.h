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

#ifndef CPULIMIT_PROCESS_SET_H
#define CPULIMIT_PROCESS_SET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

/**
 * @struct process_set
 * @brief A monitored process and optionally its descendant tree.
 *
 * proc_table owns the records across update cycles; proc_list is a
 * non-owning snapshot rebuilt each cycle.
 */
struct process_set {
    /**
     * @brief Owning PID-to-record hash table, kept across update cycles.
     */
    struct process_table *proc_table;

    /**
     * @brief Non-owning list of members active in the current cycle.
     */
    struct list *proc_list;

    /**
     * @brief Heap-allocated records (PID, start time, warning flag) for PIDs
     *        this group suspended and has not resumed.
     */
    struct list *stopped_pids;

    /**
     * @brief PID of the primary target.
     */
    pid_t target_pid;

    /**
     * @brief Start time of the primary target, or UNKNOWN_START_TIME.
     *
     * Kept outside proc_table because records of departed members are purged.
     */
    double target_start_time;

    /**
     * @brief Non-zero when a scan found the target PID held by another process.
     */
    int target_replaced;

    /**
     * @brief Non-zero to track descendants.
     */
    int include_children;

    /**
     * @brief Timestamp of the last update cycle, gating the sample interval.
     */
    struct timespec last_update;
};

/**
 * @brief Compare two process start times for approximate equality.
 *
 * @param a First start time, or UNKNOWN_START_TIME.
 * @param b Second start time, or UNKNOWN_START_TIME.
 * @return 1 when the readings are approximately equal, 0 otherwise.
 *
 * @note Shared by PID-reuse checks so both use one definition of "same
 *       process". Callers must handle UNKNOWN_START_TIME themselves.
 */
int start_time_matches(double a, double b);

/**
 * @brief Initialize a process set for monitoring.
 *
 * @param proc_set Set to initialize.
 * @param target_pid PID of the primary target.
 * @param include_children Non-zero to track descendants.
 * @return 0 on success, -1 on error; never calls exit().
 *
 * @note A NULL proc_set fails immediately; a later failure releases what
 *       has already been allocated.
 */
int init_process_set(struct process_set *proc_set, pid_t target_pid,
                     int include_children);

/**
 * @brief Release every resource owned by a process set.
 *
 * @param proc_set Set to clean up.
 * @return 0, always.
 *
 * @note Safe to call with NULL or a partially initialized set.
 */
int close_process_set(struct process_set *proc_set);

/**
 * @def RECORD_STOPPED_RESUMED
 * @brief record_stopped_pid(): the suspension was undone.
 */
#define RECORD_STOPPED_RESUMED (-1)

/**
 * @def RECORD_STOPPED_STILL_STOPPED
 * @brief record_stopped_pid(): neither a record nor an undo was possible.
 */
#define RECORD_STOPPED_STILL_STOPPED (-2)

/**
 * @brief Record a member that was just suspended with SIGSTOP.
 *
 * @param proc_set Set owning the record.
 * @param pid PID that was suspended.
 * @param start_time Start time at suspension, or UNKNOWN_START_TIME.
 * @return 0 when recorded, RECORD_STOPPED_RESUMED when the suspension was
 *         undone, RECORD_STOPPED_STILL_STOPPED when neither was possible.
 *
 * @note With no suspended-PID list the suspension is undone at once.
 */
int record_stopped_pid(struct process_set *proc_set, pid_t pid,
                       double start_time);

/**
 * @brief Resume recorded PIDs that have left the group.
 *
 * @param proc_set Set owning the records.
 * @param verbose Non-zero to name each PID that could not be resumed.
 * @return Number of PIDs that may still be stopped, ignoring ESRCH.
 *
 * @note Current members are resumed by process_set_send_signal(SIGCONT).
 *       A failed delivery keeps its record for a later retry. PID 1 records
 *       are dropped without a signal. Safe on NULL or unallocated state.
 */
int resume_stopped_pids(struct process_set *proc_set, int verbose);

/**
 * @brief Drop a suspension record without resuming the PID.
 *
 * @param proc_set Set owning the record.
 * @param pid PID to forget.
 *
 * @note Used after a failed signal: the PID may have been recycled, so a
 *       later resume could reach an unrelated process.
 */
void forget_stopped_pid(struct process_set *proc_set, pid_t pid);

/**
 * @brief Refresh the process set and recalculate CPU usage.
 *
 * @param proc_set Set to update.
 * @return 0 on success, -1 on an iterator, clock or allocation error; the
 *         caller must then break its loop so cleanup can resume the group.
 *
 * @note A NULL proc_set returns 0 immediately.
 */
int update_process_set(struct process_set *proc_set);

/**
 * @brief Aggregate CPU usage across the group.
 *
 * @param proc_set Set to query.
 * @return Sum of known cpu_usage values, or -1.0 when none is known.
 *
 * @note -1 distinguishes "unknown" from "no usage".
 */
double get_process_set_cpu_usage(const struct process_set *proc_set);

/**
 * @brief Check whether the set has no active members.
 *
 * @param proc_set Set to query.
 * @return Non-zero when proc_list is empty or proc_set is NULL.
 */
int process_set_is_empty(const struct process_set *proc_set);

/**
 * @brief Return the number of active members.
 *
 * @param proc_set Set to query.
 * @return Number of nodes in proc_list, or 0 when proc_set is NULL.
 */
size_t process_set_member_count(const struct process_set *proc_set);

/**
 * @brief Send a signal to every active member.
 *
 * @param proc_set Set to signal.
 * @param sig Signal number, e.g. SIGSTOP or SIGCONT.
 * @param verbose Non-zero to report a failed delivery, once per episode.
 * @return Number of failures that may have left a member stopped; ESRCH and
 *         a benign SIGCONT failure do not count.
 *
 * @note PID 1 is skipped. Iteration records the next node before any
 *       deletion.
 */
int process_set_send_signal(struct process_set *proc_set, int sig, int verbose);

#ifdef __cplusplus
}
#endif

#endif
