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

#ifndef CPULIMIT_PROCESS_GROUP_H
#define CPULIMIT_PROCESS_GROUP_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <time.h>

/**
 * @struct process_group
 * @brief Represents a monitored process and optionally its descendant tree
 *
 * This structure tracks a target process and optionally all its descendants
 * for CPU usage monitoring and limiting. It maintains a hashtable for fast
 * lookups and a list for iteration, along with timing information for
 * calculating CPU usage deltas.
 */
struct process_group {
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
 * @brief Initialize a process group for monitoring and CPU limiting
 * @param proc_group Pointer to uninitialized process_group structure to set up
 * @param target_pid PID of the primary process to monitor
 * @param include_children Non-zero to monitor descendants, zero for target only
 * @return 0 on success, -1 if proc_group is NULL; exits on other errors
 *
 * This function:
 * 1. Allocates and initializes the process hashtable (PROCESS_TABLE_HASHSIZE
 *    buckets)
 * 2. Allocates and initializes the process list
 * 3. Records the current time as baseline for CPU calculations
 * 4. Performs initial update to populate the process list
 *
 * @note Returns -1 immediately if proc_group is NULL
 * @note Calls exit(EXIT_FAILURE) on memory allocation or timing errors
 * @note After return, proc_group is fully initialized and ready for use
 */
int init_process_group(struct process_group *proc_group, pid_t target_pid,
                       int include_children);

/**
 * @brief Release all resources associated with a process group
 * @param proc_group Pointer to the process_group structure to clean up
 * @return 0 on success (always succeeds)
 *
 * This function:
 * 1. Clears and frees the process list
 * 2. Destroys and frees the process hashtable
 * 3. Sets pointers to NULL and zeros numeric fields for safety
 *
 * @note Safe to call with NULL proc_group (returns 0 immediately)
 * @note Safe to call even if proc_group is partially initialized (NULLs are
 *       handled)
 * @note Does not send any signals to processes; they continue running
 * @note After return, proc_group fields should not be accessed without
 *       re-initialization
 */
int close_process_group(struct process_group *proc_group);

/**
 * @brief Refresh process group state and recalculate CPU usage
 * @param proc_group Pointer to the process_group structure to update
 *
 * This function performs a complete refresh of the process group:
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
 * @note Safe to call with NULL proc_group (returns immediately)
 * @note Should be called periodically (e.g., every 100ms) during CPU limiting
 * @note Calls exit(EXIT_FAILURE) on critical errors (iterator init, time
 *       retrieval)
 */
void update_process_group(struct process_group *proc_group);

/**
 * @brief Calculate aggregate CPU usage across all processes in the group
 * @param proc_group Pointer to the process_group structure to query
 * @return Sum of CPU usage values for all processes with known usage, or
 *         -1.0 if no processes have valid CPU measurements yet or if
 *         proc_group is NULL
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
 * @note Thread-safe if proc_group is not being modified concurrently
 * @note Safe to call with NULL proc_group (returns -1)
 */
double get_process_group_cpu_usage(const struct process_group *proc_group);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PROCESS_GROUP_H */
