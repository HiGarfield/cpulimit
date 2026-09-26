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
 * @brief A monitored process and optionally its descendant tree
 *
 * proc_table OWNS the process records and survives update cycles, so PID-reuse
 * detection and the CPU-usage EMA retain each PID's previous sample.
 * proc_list is a NON-OWNING per-cycle snapshot rebuilt every cycle for
 * iteration and signal delivery.
 */
struct process_set {
    /**
     * @brief Owning PID->record hashtable (O(1) lookup).
     *
     * Allocates records on discovery and frees them when a PID leaves the
     * group; survives update cycles so PID-reuse detection and EMA keep the
     * previous cpu_time sample.
     */
    struct process_table *proc_table;

    /**
     * @brief Non-owning list of processes active in the current cycle.
     *
     * Holds borrowed pointers into proc_table and is rebuilt from scratch by
     * update_process_set(); used for iteration and SIGSTOP/SIGCONT delivery.
     */
    struct list *proc_list;

    /**
     * @brief PIDs this group suspended with SIGSTOP and has not resumed yet.
     *
     * Each element is a heap-allocated {pid, start_time} record owned by the
     * list. It outlives a cycle because proc_list is rebuilt and a suspended
     * process that leaves the group would otherwise never get its SIGCONT;
     * start_time guards against resuming a recycled PID.
     */
    struct list *stopped_pids;

    /**
     * @brief PID of the primary target process (root of the monitored tree).
     */
    pid_t target_pid;

    /**
     * @brief Start time target_pid had when the group was created, or
     *        UNKNOWN_START_TIME if unavailable.
     *
     * Stored here (not in a proc_table record) because records of departed
     * members are purged each cycle; this lets a recycled PID be rejected.
     */
    double target_start_time;

    /**
     * @brief Non-zero to monitor target and descendants, zero for target only.
     */
    int include_children;

    /**
     * @brief Timestamp of the most recent update, for CPU-usage dt.
     */
    struct timespec last_update;
};

/**
 * @brief Initialize a process set for monitoring and CPU limiting
 *
 * @param proc_set Pointer to uninitialized process_set structure to set up
 * @param target_pid PID of the primary process to monitor
 * @param include_children Non-zero to monitor descendants, zero for target only
 * @return 0 on success, -1 on error; it never calls exit(), so the caller can
 *         resume the group and exit cleanly instead of stranding a stopped
 *         process
 *
 * @note Returns -1 immediately if proc_set is NULL, and releases partially
 *       allocated resources on any later failure
 */
int init_process_set(struct process_set *proc_set, pid_t target_pid,
                     int include_children);

/**
 * @brief Release all resources associated with a process set
 *
 * @param proc_set Pointer to the process_set structure to clean up
 * @return 0 on success (always succeeds)
 *
 * @note Safe to call with NULL proc_set or a partially initialized one
 */
int close_process_set(struct process_set *proc_set);

/**
 * @brief Record that a member was just suspended with SIGSTOP
 *
 * @param proc_set Pointer to the process set structure
 * @param pid PID that was successfully sent SIGSTOP
 * @param start_time Start time of pid at suspension, from
 *        get_process_start_time(); pass UNKNOWN_START_TIME when the platform
 *        cannot report one, which disables the recycle check for this PID
 * @return 0 when recorded, -1 when not: the process was then already resumed
 *         and the caller must not treat it as suspended by this group
 *
 * @note Safe to call with NULL proc_set or an unallocated suspended-PID list;
 *       the call is then a -1 no-op
 */
int record_stopped_pid(struct process_set *proc_set, pid_t pid,
                       double start_time);

/**
 * @brief Resume every PID recorded by record_stopped_pid() and empty the list
 *
 * @param proc_set Pointer to the process set structure
 * @return Number of recorded PIDs that could not be resumed for a reason other
 *         than ESRCH (they may have been left stopped and the shutdown report
 *         must treat them like a failed resume of a current member)
 *
 * @note Safe to call with NULL proc_set or an unallocated suspended-PID list;
 *       the call is then a no-op returning 0
 */
int resume_stopped_pids(struct process_set *proc_set);

/**
 * @brief Drop a PID from the suspension record without resuming it
 *
 * @param proc_set Pointer to the process set structure
 * @param pid PID to forget
 *
 * Called when a signal to the process failed, so it can no longer be
 * controlled and there is no suspension left to undo. Resuming it later would
 * be unsafe: the PID may have been recycled and the resume would reach an
 * unrelated process.
 */
void forget_stopped_pid(struct process_set *proc_set, pid_t pid);

/**
 * @brief Refresh process set state and recalculate CPU usage
 *
 * @param proc_set Pointer to the process_set structure to update
 * @return 0 on success, -1 on a critical error (iterator, clock or
 *         allocation); the caller must then break its limiting loop rather
 *         than exit, so cleanup can resume whatever is stopped
 *
 * @note Safe to call with NULL proc_set (returns 0 immediately)
 */
int update_process_set(struct process_set *proc_set);

/**
 * @brief Aggregate CPU usage across all processes in the group
 *
 * @param proc_set Pointer to the process_set structure to query
 * @return Sum of cpu_usage for members with a known measurement, or -1.0 if
 *         none are known yet or proc_set is NULL
 *
 * @note Returns -1 rather than 0 to distinguish "no usage" from "unknown"
 */
double get_process_set_cpu_usage(const struct process_set *proc_set);

/**
 * @brief Check whether the process set currently has no active members
 *
 * @param proc_set Pointer to the process_set structure to query
 * @return Non-zero if proc_list is empty or proc_set is NULL
 */
int process_set_is_empty(const struct process_set *proc_set);

/**
 * @brief Return the number of active members in the process set
 *
 * @param proc_set Pointer to the process_set structure to query
 * @return Number of nodes in proc_list, or 0 if proc_set is NULL
 */
size_t process_set_member_count(const struct process_set *proc_set);

/**
 * @brief Send a signal to every active member of the process set
 *
 * @param proc_set Pointer to the process set structure
 * @param sig Signal number to send (e.g., SIGSTOP, SIGCONT)
 * @param verbose Retained for API compatibility; failure reporting is throttled
 *                per member by the stop_warned/cont_warned/resume_warned flags
 * @return Number of deliveries that failed and may have left a member
 * suspended. A failed SIGCONT for a member this group never suspended is benign
 * and does not count; an ESRCH (process gone) never counts.
 *
 * @note Safe iteration: stores the next node before a potential deletion
 */
int process_set_send_signal(struct process_set *proc_set, int sig, int verbose);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PROCESS_SET_H */
