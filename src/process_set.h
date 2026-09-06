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

#ifndef CPULIMIT_PROCESS_SET_H
#define CPULIMIT_PROCESS_SET_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct timespec requires _POSIX_C_SOURCE >= 199309L to be visible in
 * strict C89 mode.  When a .c file defines _GNU_SOURCE before including
 * this header, all POSIX features are already enabled.  This guard
 * ensures the header is self-contained when analyzed or compiled
 * standalone (e.g., by clang-tidy).
 */
#if !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <sys/types.h>
#include <time.h>

/**
 * @struct process_set
 * @brief Represents a monitored process and optionally its descendant tree
 *
 * This structure tracks a target process and optionally all its descendants
 * for CPU usage monitoring and limiting. It maintains a hashtable for fast
 * lookups and a list for iteration, along with timing information for
 * calculating CPU usage deltas.
 */
struct process_set {
    /**
     * Hashtable mapping PIDs to process structures for O(1) lookup.
     * Used to detect new processes, reused PIDs, and track historical data.
     */
    struct process_table *proc_table;

    /**
     * Linked list of currently active processes in this group.
     * Rebuilt on each update by scanning /proc (or equivalent).
     * Contains pointers to process structures stored in proc_table.
     */
    struct list *proc_list;

    /**
     * PIDs that this group has suspended with SIGSTOP and has not resumed
     * yet. Each element is a heap-allocated pid_t owned by the list.
     *
     * The list outlives a single update cycle on purpose: proc_list is
     * rebuilt from scratch by update_process_set(), and a process that
     * stops matching the group while suspended (for example a descendant
     * that is re-parented away when its monitored ancestor exits) would
     * otherwise never receive the SIGCONT that undoes the SIGSTOP.
     */
    struct list *stopped_pids;

    /**
     * PID of the primary target process.
     * This is the root of the process tree being monitored.
     */
    pid_t target_pid;

    /**
     * Flag controlling descendant tracking:
     * - Non-zero: monitor target and all descendant processes (recursive)
     * - Zero: monitor only the target process itself
     */
    int include_children;

    /**
     * Timestamp of the most recent update operation.
     * Used to calculate time deltas (dt) for CPU usage computation.
     * Measured via clock_gettime() or equivalent high-resolution timer.
     */
    struct timespec last_update;
};

/**
 * @brief Initialize a process set for monitoring and CPU limiting
 * @param proc_set Pointer to uninitialized process_set structure to set up
 * @param target_pid PID of the primary process to monitor
 * @param include_children Non-zero to monitor descendants, zero for target only
 * @return 0 on success, -1 if proc_set is NULL; exits on other errors
 *
 * This function:
 * 1. Allocates and initializes the process hashtable (PROCESS_TABLE_HASHSIZE
 *    buckets)
 * 2. Allocates and initializes the process list
 * 3. Allocates and initializes the suspended-PID list
 * 4. Records the current time as baseline for CPU calculations
 * 5. Performs initial update to populate the process list
 *
 * @note Returns -1 immediately if proc_set is NULL
 * @note Calls exit(EXIT_FAILURE) on memory allocation or timing errors
 * @note After return, proc_set is fully initialized and ready for use
 */
int init_process_set(struct process_set *proc_set, pid_t target_pid,
                     int include_children);

/**
 * @brief Release all resources associated with a process set
 * @param proc_set Pointer to the process_set structure to clean up
 * @return 0 on success (always succeeds)
 *
 * This function:
 * 1. Clears and frees the process list
 * 2. Destroys and frees the suspended-PID list
 * 3. Destroys and frees the process hashtable
 * 4. Sets pointers to NULL and zeros numeric fields for safety
 *
 * @note Safe to call with NULL proc_set (returns 0 immediately)
 * @note Safe to call even if proc_set is partially initialized (NULLs are
 *       handled)
 * @note Does not send any signals to processes; they continue running
 * @note After return, proc_set fields should not be accessed without
 *       re-initialization
 */
int close_process_set(struct process_set *proc_set);

/**
 * @brief Record that a member of the group has just been suspended
 * @param proc_set Pointer to the process set structure
 * @param pid PID that was successfully sent SIGSTOP
 *
 * proc_list is rebuilt from scratch by update_process_set(), so a process
 * can cease to be a member of the group while it is still suspended: a
 * descendant, for instance, is re-parented away when its monitored ancestor
 * exits, and is_child_of() then no longer matches it.  Recording the PID
 * keeps the suspension undoable after the process has left proc_list.
 *
 * @note Safe to call with NULL proc_set or a group whose suspended-PID
 *       list has not been allocated; the call is then a no-op
 * @note Skips the record if the pid_t cannot be allocated; the process is
 *       still a group member, so the regular resume round reaches it
 */
void record_stopped_pid(struct process_set *proc_set, pid_t pid);

/**
 * @brief Resume every PID recorded by record_stopped_pid() and empty the list
 * @param proc_set Pointer to the process set structure
 *
 * Sends SIGCONT to every recorded PID that has left the group and frees the
 * list.  Group members are resumed by the regular resume round, which walks
 * proc_list, so they are deliberately not signalled twice.  Used both for
 * the regular resume round and for the final cleanup, so that processes
 * which left the group while suspended are resumed as well instead of
 * staying suspended forever.
 *
 * @note Safe to call with NULL proc_set or a group whose suspended-PID
 *       list has not been allocated; the call is then a no-op
 */
void resume_stopped_pids(struct process_set *proc_set);

/**
 * @brief Drop a PID from the suspension record without resuming it
 * @param proc_set Pointer to the process set structure
 * @param pid PID to forget
 *
 * Called when a signal to a tracked process failed, which means the
 * process can no longer be controlled and there is no suspension left to
 * undo. Resuming it later would be worse than pointless: the PID may
 * already have been recycled, and resume_stopped_pids() would then send
 * the resume to an unrelated process.
 *
 * @note Safe to call with NULL proc_set or a group whose suspended-PID
 *       list has not been allocated; the call is then a no-op
 * @note Removes every record for the PID, so the list cannot keep a
 *       duplicate entry behind
 */
void forget_stopped_pid(struct process_set *proc_set, pid_t pid);

/**
 * @brief Refresh process set state and recalculate CPU usage
 * @param proc_set Pointer to the process_set structure to update
 *
 * This function performs a complete refresh of the process set:
 * 1. Scans /proc (or platform equivalent) for current target and descendants
 * 2. Updates the process list, removing terminated processes from tracking
 * 3. Calculates CPU usage for each process using exponential moving average
 * 4. Handles edge cases: PID reuse, clock skew, insufficient time delta
 * 5. Updates last_update timestamp if sufficient time has elapsed or if
 *    time moved backwards (to establish a new baseline)
 *
 * CPU usage calculation:
 * - Requires minimum time delta (CPU_MIN_DELTA_MS = 20ms) for accuracy
 * - Uses exponential smoothing: cpu = (1-alpha)*old + alpha*sample,
 *   alpha = CPU_EMA_ALPHA = 0.08
 * - Detects PID reuse when cpu_time decreases (resets history)
 * - Handles backward time jumps (system clock adjustment)
 * - New processes have cpu_usage=-1 until first valid measurement
 *
 * @return 0 on success. -1 on critical errors (iterator init/close, time
 *         retrieval, memory allocation). The caller must not call exit() on
 *         this path without first resuming any stopped processes; use the
 *         return value to break out of the limiting loop so that SIGCONT is
 *         sent by the cleanup code
 * @note Safe to call with NULL proc_set (returns 0 immediately)
 * @note Should be called periodically (e.g., every 100ms) during CPU limiting
 * @note Stale hash table entries are purged even when the iterator fails to
 *       close, so proc_table never retains exited processes across cycles
 */
int update_process_set(struct process_set *proc_set);

/**
 * @brief Calculate aggregate CPU usage across all processes in the group
 * @param proc_set Pointer to the process_set structure to query
 * @return Sum of CPU usage values for all processes with known usage, or
 *         -1.0 if no processes have valid CPU measurements yet or if
 *         proc_set is NULL
 *
 * CPU usage is expressed as a fraction of total system CPU capacity:
 * - 0.0 = idle
 * - 1.0 = fully utilizing one CPU core
 * - N = fully utilizing N CPU cores (on multi-core systems)
 *
 * The function:
 * 1. Iterates through all processes in proc_list
 * 2. Sums cpu_usage for processes with valid measurements (cpu_usage >= 0)
 * 3. Returns -1 if all processes have unknown usage (first update cycle)
 *
 * @note Returns -1 rather than 0 to distinguish "no usage" from "unknown"
 * @note Thread-safe if proc_set is not being modified concurrently
 * @note Safe to call with NULL proc_set (returns -1)
 */
double get_process_set_cpu_usage(const struct process_set *proc_set);

/**
 * @brief Check whether the process set currently has no active members
 * @param proc_set Pointer to the process_set structure to query
 * @return Non-zero if proc_list is empty or proc_set is NULL
 */
int process_set_is_empty(const struct process_set *proc_set);

/**
 * @brief Return the number of active members in the process set
 * @param proc_set Pointer to the process_set structure to query
 * @return Number of nodes in proc_list, or 0 if proc_set is NULL
 */
size_t process_set_member_count(const struct process_set *proc_set);

/**
 * @brief Send a signal to every active member of the process set
 * @param proc_set Pointer to the process set structure
 * @param sig Signal number to send (e.g., SIGSTOP, SIGCONT)
 * @param verbose If non-zero, print errors when signal delivery fails
 *
 * Iterates through all processes in the group and sends the specified
 * signal.  If signal delivery fails (e.g., process terminated), the
 * process is removed from the group and from the process table to avoid
 * repeated errors.  Successful SIGSTOP delivery is recorded so that the
 * suspension can always be undone; SIGCONT additionally resumes processes
 * that were recorded earlier but have since left the group.
 *
 * @note Safe iteration: stores next node before potential deletion
 */
void process_set_send_signal(struct process_set *proc_set, int sig,
                             int verbose);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PROCESS_SET_H */
