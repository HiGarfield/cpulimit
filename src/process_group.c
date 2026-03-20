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

#include "process_group.h"

#include "list.h"
#include "process_iterator.h"
#include "process_table.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/**
 * @def PROCESS_TABLE_HASHSIZE
 * @brief Number of hash buckets for the process hashtable
 *
 * The hash table uses separate chaining for collision resolution.
 * A larger size reduces collision probability at the cost of more memory.
 * 2048 buckets is sufficient for typical process counts while keeping
 * memory overhead low.
 */
#define PROCESS_TABLE_HASHSIZE 2048

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
                       int include_children) {
    if (proc_group == NULL) {
        return -1;
    }
    memset(proc_group, 0, sizeof(*proc_group));
    /* Allocate and initialize hashtable for fast process lookup by PID */
    proc_group->proc_table =
        (struct process_table *)malloc(sizeof(struct process_table));
    if (proc_group->proc_table == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
    init_process_table(proc_group->proc_table, PROCESS_TABLE_HASHSIZE);
    proc_group->target_pid = target_pid;
    proc_group->include_children = include_children;

    /* Allocate and initialize linked list for process iteration */
    proc_group->proc_list = (struct list *)malloc(sizeof(struct list));
    if (proc_group->proc_list == NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        close_process_group(proc_group);
        exit(EXIT_FAILURE);
    }
    init_list(proc_group->proc_list);

    /* Record baseline timestamp for CPU usage calculation */
    if (get_current_time(&proc_group->last_update) != 0) {
        perror("get_current_time");
        close_process_group(proc_group);
        exit(EXIT_FAILURE);
    }
    /* Perform initial scan to populate process list */
    update_process_group(proc_group);
    return 0;
}

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
int close_process_group(struct process_group *proc_group) {
    if (proc_group == NULL) {
        return 0;
    }
    if (proc_group->proc_list != NULL) {
        /*
         * Use clear_list (not destroy_list) because the data pointers in
         * proc_list are the same process structs stored in proc_table.
         * destroy_process_table below will free all data exactly once.
         * Using destroy_list here would double-free the process structs.
         */
        clear_list(proc_group->proc_list);
        free(proc_group->proc_list);
        proc_group->proc_list = NULL;
    }

    if (proc_group->proc_table != NULL) {
        destroy_process_table(proc_group->proc_table);
        free(proc_group->proc_table);
        proc_group->proc_table = NULL;
    }

    /* Zero out remaining fields to prevent stale data after close */
    memset(proc_group, 0, sizeof(*proc_group));

    return 0;
}

/**
 * @brief Create a deep copy of a process structure
 * @param proc Pointer to the source process structure to duplicate
 * @return Pointer to newly allocated process structure containing copied data
 *
 * Allocates memory for a new process structure and copies all fields from
 * the source. The caller is responsible for freeing the returned pointer.
 *
 * @note Calls exit(EXIT_FAILURE) if memory allocation fails
 */
static struct process *process_dup(const struct process *proc) {
    struct process *new_proc;
    new_proc = (struct process *)malloc(sizeof(struct process));
    if (new_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        exit(EXIT_FAILURE);
    }
    *new_proc = *proc;
    return new_proc;
}

/**
 * @def CPU_EMA_ALPHA
 * @brief Smoothing factor for exponential moving average of CPU usage
 *
 * Value range: (0, 1)
 * - Lower values (e.g., 0.05): more smoothing, slower response to changes
 * - Higher values (e.g., 0.2): less smoothing, faster response to changes
 * Formula: new_value = (1-CPU_EMA_ALPHA) * old_value + CPU_EMA_ALPHA * sample
 */
#define CPU_EMA_ALPHA 0.08

/**
 * @def CPU_MIN_DELTA_MS
 * @brief Minimum time delta (milliseconds) required for valid CPU usage
 *        calculation
 *
 * Updates with smaller time differences are skipped to avoid:
 * - Division by very small numbers (numerical instability)
 * - Amplification of measurement noise
 * - Excessive sensitivity to timer resolution
 */
#define CPU_MIN_DELTA_MS 20

/**
 * @brief Update the CPU usage of an existing tracked process entry.
 * @param proc      The stored process entry to update (modified in place).
 * @param scan_proc Fresh snapshot of the same process from the iterator.
 * @param elapsed_ms Milliseconds elapsed since the last update cycle.
 * @param ncpu      Number of available CPU cores (used to cap the sample).
 *
 * Handles four mutually exclusive cases:
 * - PID reuse (scan_proc->cpu_time < proc->cpu_time): resets all fields.
 * - Backward clock (elapsed_ms < 0): updates ppid and cpu_time, marks
 *   usage unknown so the next cycle starts from a clean baseline.
 * - Short interval (elapsed_ms < CPU_MIN_DELTA_MS): updates ppid only;
 *   holds cpu_time so the next valid update spans the full accumulated
 *   delta.
 * - Normal: computes a CPU sample, applies exponential moving average,
 *   and updates ppid and cpu_time.
 */
static void update_existing_process_entry(struct process *proc,
                                          const struct process *scan_proc,
                                          double elapsed_ms, int ncpu) {
    double sample;
    if (scan_proc->cpu_time < proc->cpu_time) {
        /*
         * CPU time decreased: PID has been reused for a new process.
         * Reset all historical data.
         */
        *proc = *scan_proc;
        /* Mark CPU usage as unknown for new process */
        proc->cpu_usage = -1;
        return;
    }
    /*
     * In all non-reuse cases the parent PID is always updated to the
     * current value; it is independent of timing accuracy.
     */
    proc->ppid = scan_proc->ppid;
    if (elapsed_ms < 0) {
        /*
         * Time moved backwards (system clock adjustment, NTP
         * correction). Update cpu_time but don't calculate usage this
         * cycle.
         */
        proc->cpu_time = scan_proc->cpu_time;
        proc->cpu_usage = -1;
        return;
    }
    if (elapsed_ms < CPU_MIN_DELTA_MS) {
        /* Time delta too small for accurate CPU measurement; keep
         * cpu_time unchanged so the next valid update accumulates
         * the full delta over the interval. */
        return;
    }
    /*
     * Calculate CPU usage sample:
     * sample = (delta_cputime / delta_walltime)
     * This represents the fraction of one CPU core used.
     */
    sample = (scan_proc->cpu_time - proc->cpu_time) / elapsed_ms;
    /* Cap sample at total CPU capacity (shouldn't exceed N cores) */
    sample = MIN(sample, (double)ncpu);
    if (proc->cpu_usage < 0) {
        /* First valid measurement: initialize directly */
        proc->cpu_usage = sample;
    } else {
        /*
         * Apply exponential moving average for smooth tracking:
         * new = (1-alpha)*old + alpha*sample
         * This reduces noise while remaining responsive to changes.
         */
        proc->cpu_usage =
            (1.0 - CPU_EMA_ALPHA) * proc->cpu_usage + CPU_EMA_ALPHA * sample;
    }
    /* Update stored CPU time for next delta calculation */
    proc->cpu_time = scan_proc->cpu_time;
}

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
void update_process_group(struct process_group *proc_group) {
    struct process_iterator iter;
    struct process *scan_proc;
    struct process_filter filter;
    struct timespec now;
    double elapsed_ms;
    int ncpu;
    if (proc_group == NULL) {
        return;
    }
    ncpu = get_ncpu(); /* get_ncpu() caches its result across calls */

    /* Get current timestamp for delta calculation */
    if (get_current_time(&now) != 0) {
        perror("get_current_time");
        exit(EXIT_FAILURE);
    }
    scan_proc = (struct process *)malloc(sizeof(struct process));
    if (scan_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for scan_proc\n");
        exit(EXIT_FAILURE);
    }
    /* Calculate elapsed time since last update (milliseconds) */
    elapsed_ms = timediff_in_ms(&now, &proc_group->last_update);

    /* Configure iterator to scan target process and optionally descendants */
    filter.pid = proc_group->target_pid;
    filter.include_children = proc_group->include_children;
    filter.read_cmd = 0;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(scan_proc);
        exit(EXIT_FAILURE);
    }

    /* Clear process list (will be rebuilt from scratch) */
    clear_list(proc_group->proc_list);

    /* Scan currently running processes and update tracking data */
    while (get_next_process(&iter, scan_proc) != -1) {
        struct process *proc =
            find_in_process_table(proc_group->proc_table, scan_proc->pid);
        if (proc == NULL) {
            /* New process detected: add to hashtable and list */
            proc = process_dup(scan_proc);
            /* Mark CPU usage as unknown until we have a time delta */
            proc->cpu_usage = -1;
            add_to_process_table(proc_group->proc_table, proc);
            add_elem(proc_group->proc_list, proc);
        } else {
            /* Existing process: re-add to list for this cycle */
            add_elem(proc_group->proc_list, proc);
            update_existing_process_entry(proc, scan_proc, elapsed_ms, ncpu);
        }
    }
    free(scan_proc);
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
        exit(EXIT_FAILURE);
    }

    /* Remove hash table entries for processes that are no longer running */
    remove_stale_from_process_table(proc_group->proc_table,
                                    proc_group->proc_list);

    /*
     * Update timestamp only if sufficient time passed for CPU calculation
     * or if time moved backwards (to establish new baseline).
     */
    if (elapsed_ms < 0 || elapsed_ms >= CPU_MIN_DELTA_MS) {
        proc_group->last_update = now;
    }
}

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
double get_process_group_cpu_usage(const struct process_group *proc_group) {
    const struct list_node *node;
    double cpu_usage = -1;
    if (proc_group == NULL) {
        return -1;
    }
    for (node = first_node(proc_group->proc_list); node != NULL;
         node = node->next) {
        const struct process *proc = (const struct process *)node->data;
        /* Skip NULL-data nodes (should not occur but defensive) */
        if (proc == NULL) {
            continue;
        }
        /* Skip processes without valid CPU measurements yet */
        if (proc->cpu_usage < 0) {
            continue;
        }
        /* Initialize sum on first valid process */
        if (cpu_usage < 0) {
            cpu_usage = 0;
        }
        cpu_usage += proc->cpu_usage;
    }
    return cpu_usage;
}
