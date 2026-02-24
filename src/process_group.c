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
 * @brief Check if a process exists and can be controlled by cpulimit
 * @param pid Process ID to search for
 * @return Positive PID if process exists and can be signaled
 *         (kill(pid,0)==0), negative -PID if process exists but permission
 *         denied (errno==EPERM), 0 if process does not exist (errno==ESRCH or
 *         invalid PID)
 *
 * Uses kill(pid, 0) as a lightweight probe to test process existence and
 * signal permission without actually sending a signal. This is the standard
 * POSIX method for checking process liveness and accessibility.
 */
pid_t find_process_by_pid(pid_t pid) {
    /* Reject invalid PIDs (must be positive) */
    if (pid <= 0) {
        return 0;
    }
    /* Attempt to send null signal (doesn't actually signal, just checks
     * permission) */
    if (kill(pid, 0) == 0) {
        return pid;
    }
    /* Process exists but we lack permission to signal it */
    if (errno == EPERM) {
        return -pid;
    }
    /* Process does not exist (errno is ESRCH or other error) */
    return 0;
}

/**
 * @brief Find a running process by its executable name or path
 * @param process_name Name or absolute path of the executable to search for
 * @return Positive PID if found and accessible, negative -PID if found but
 *         permission denied, 0 if not found or invalid name
 *
 * Behavior depends on whether process_name is an absolute path:
 * - If process_name starts with '/': compares full absolute paths
 * - Otherwise: compares only the basename (executable name without directory)
 *
 * When multiple matches exist, selects the first process found, or if one is
 * an ancestor of another, prefers the ancestor. This heuristic helps ensure
 * that if a parent process spawns children with the same name, the parent is
 * chosen.
 *
 * @note Iterates through all processes in the system, which may be slow on
 *       systems with many processes. For known PIDs, use find_process_by_pid().
 * @note Calls exit(EXIT_FAILURE) on critical errors (e.g., memory allocation
 *       failure or iterator initialization failure)
 */
pid_t find_process_by_name(const char *process_name) {
    int found = 0;
    pid_t pid = 0;
    struct process_iterator it;
    struct process_filter filter;
    struct process *proc;
    int full_path_cmp;
    const char *process_cmp_name;

    if (process_name == NULL || process_name[0] == '\0') {
        return 0;
    }

    /*
     * Determine comparison mode:
     * - Absolute path (starts with '/'): compare full paths
     * - Relative path/name: compare basenames only
     */
    full_path_cmp = process_name[0] == '/';
    process_cmp_name =
        full_path_cmp ? process_name : file_basename(process_name);
    if ((proc = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process\n");
        exit(EXIT_FAILURE);
    }

    /* Configure iterator to scan all processes and read command names */
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&it, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(proc);
        exit(EXIT_FAILURE);
    }

    /* Scan all processes to find matching executable */
    while (get_next_process(&it, proc) != -1) {
        const char *cmd_cmp_name =
            full_path_cmp ? proc->command : file_basename(proc->command);
        /* Check if this process matches the target name */
        if (strcmp(cmd_cmp_name, process_cmp_name) == 0) {
            /*
             * Select this PID if:
             * - No match found yet (!found), OR
             * - This process is an ancestor of the previous match
             * This heuristic prefers older/parent processes over newer/child
             * ones
             */
            if (!found || is_child_of(pid, proc->pid)) {
                pid = proc->pid;
                found = 1;
            }
        }
    }
    free(proc);
    if (close_process_iterator(&it) != 0) {
        exit(EXIT_FAILURE);
    }

    /* Verify the found process still exists and is accessible */
    return found ? find_process_by_pid(pid) : 0;
}

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
 * @param pgroup Pointer to uninitialized process_group structure to set up
 * @param target_pid PID of the primary process to monitor
 * @param include_children Non-zero to monitor descendants, zero for target only
 * @return 0 on success (always succeeds or exits the program)
 *
 * This function:
 * 1. Allocates and initializes the process hashtable (PROCESS_TABLE_HASHSIZE
 *    buckets)
 * 2. Allocates and initializes the process list
 * 3. Records the current time as baseline for CPU calculations
 * 4. Performs initial update to populate the process list
 *
 * @note Calls exit(EXIT_FAILURE) on memory allocation or timing errors
 * @note After return, pgroup is fully initialized and ready for use
 */
int init_process_group(struct process_group *pgroup, pid_t target_pid,
                       int include_children) {
    /* Allocate and initialize hashtable for fast process lookup by PID */
    if ((pgroup->proctable = (struct process_table *)malloc(
             sizeof(struct process_table))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
    process_table_init(pgroup->proctable, PROCESS_TABLE_HASHSIZE);
    pgroup->target_pid = target_pid;
    pgroup->include_children = include_children;

    /* Allocate and initialize linked list for process iteration */
    if ((pgroup->proclist = (struct list *)malloc(sizeof(struct list))) ==
        NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        process_table_destroy(pgroup->proctable);
        free(pgroup->proctable);
        pgroup->proctable = NULL;
        exit(EXIT_FAILURE);
    }
    init_list(pgroup->proclist);

    /* Record baseline timestamp for CPU usage calculation */
    if (get_current_time(&pgroup->last_update) != 0) {
        close_process_group(pgroup);
        exit(EXIT_FAILURE);
    }
    /* Perform initial scan to populate process list */
    update_process_group(pgroup);
    return 0;
}

/**
 * @brief Release all resources associated with a process group
 * @param pgroup Pointer to the process_group structure to clean up
 * @return 0 on success (always succeeds)
 *
 * This function:
 * 1. Clears and frees the process list
 * 2. Destroys and frees the process hashtable
 * 3. Sets pointers to NULL and zeros numeric fields for safety
 *
 * @note Safe to call with NULL pgroup (returns 0 immediately)
 * @note Safe to call even if pgroup is partially initialized (NULLs are
 *       handled)
 * @note Does not send any signals to processes; they continue running
 * @note After return, pgroup fields should not be accessed without
 *       re-initialization
 */
int close_process_group(struct process_group *pgroup) {
    if (pgroup == NULL) {
        return 0;
    }
    if (pgroup->proclist != NULL) {
        /*
         * Use clear_list (not destroy_list) because the data pointers in
         * proclist are the same process structs stored in proctable.
         * process_table_destroy below will free all data exactly once.
         * Using destroy_list here would double-free the process structs.
         */
        clear_list(pgroup->proclist);
        free(pgroup->proclist);
        pgroup->proclist = NULL;
    }

    if (pgroup->proctable != NULL) {
        process_table_destroy(pgroup->proctable);
        free(pgroup->proctable);
        pgroup->proctable = NULL;
    }

    /* Zero out remaining fields to prevent stale data after close */
    memset(pgroup, 0, sizeof(*pgroup));

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
    struct process *p;
    if ((p = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        exit(EXIT_FAILURE);
    }
    *p = *proc;
    return p;
}

/**
 * @def ALPHA
 * @brief Smoothing factor for exponential moving average of CPU usage
 *
 * Value range: (0, 1)
 * - Lower values (e.g., 0.05): more smoothing, slower response to changes
 * - Higher values (e.g., 0.2): less smoothing, faster response to changes
 * Formula: new_value = (1-ALPHA) * old_value + ALPHA * sample
 */
#define ALPHA 0.08

/**
 * @def MIN_DT
 * @brief Minimum time delta (milliseconds) required for valid CPU usage
 *        calculation
 *
 * Updates with smaller time differences are skipped to avoid:
 * - Division by very small numbers (numerical instability)
 * - Amplification of measurement noise
 * - Excessive sensitivity to timer resolution
 */
#define MIN_DT 20

/**
 * @brief Refresh process group state and recalculate CPU usage
 * @param pgroup Pointer to the process_group structure to update
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
 * - Requires minimum time delta (MIN_DT = 20ms) for accuracy
 * - Uses exponential smoothing: cpu = (1-alpha)*old + alpha*sample, alpha=0.08
 * - Detects PID reuse when cputime decreases (resets history)
 * - Handles backward time jumps (system clock adjustment)
 * - New processes have cpu_usage=-1 until first valid measurement
 *
 * @note Safe to call with NULL pgroup (returns immediately)
 * @note Should be called periodically (e.g., every 100ms) during CPU limiting
 * @note Calls exit(EXIT_FAILURE) on critical errors (iterator init, time
 *       retrieval)
 */
void update_process_group(struct process_group *pgroup) {
    struct process_iterator it;
    struct process *tmp_process;
    struct process_filter filter;
    struct timespec now;
    double dt;
    int ncpu;
    if (pgroup == NULL) {
        return;
    }
    ncpu = get_ncpu(); /* Cached: compute once per call */

    /* Get current timestamp for delta calculation */
    if (get_current_time(&now) != 0) {
        exit(EXIT_FAILURE);
    }
    if ((tmp_process = (struct process *)malloc(sizeof(struct process))) ==
        NULL) {
        fprintf(stderr, "Memory allocation failed for tmp_process\n");
        exit(EXIT_FAILURE);
    }
    /* Calculate elapsed time since last update (milliseconds) */
    dt = timediff_in_ms(&now, &pgroup->last_update);

    /* Configure iterator to scan target process and optionally descendants */
    filter.pid = pgroup->target_pid;
    filter.include_children = pgroup->include_children;
    filter.read_cmd = 0;
    if (init_process_iterator(&it, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(tmp_process);
        exit(EXIT_FAILURE);
    }

    /* Clear process list (will be rebuilt from scratch) */
    clear_list(pgroup->proclist);

    /* Scan currently running processes and update tracking data */
    while (get_next_process(&it, tmp_process) != -1) {
        struct process *p =
            process_table_find(pgroup->proctable, tmp_process->pid);
        if (p == NULL) {
            /* New process detected: add to hashtable and list */
            p = process_dup(tmp_process);
            /* Mark CPU usage as unknown until we have a time delta */
            p->cpu_usage = -1;
            process_table_add(pgroup->proctable, p);
            add_elem(pgroup->proclist, p);
        } else {
            double sample;
            /* Existing process: re-add to list for this cycle */
            add_elem(pgroup->proclist, p);
            if (tmp_process->cputime < p->cputime) {
                /*
                 * CPU time decreased: PID has been reused for a new process.
                 * Reset all historical data.
                 */
                *p = *tmp_process;
                /* Mark CPU usage as unknown for new process */
                p->cpu_usage = -1;
                continue;
            }
            if (dt < 0) {
                /*
                 * Time moved backwards (system clock adjustment, NTP
                 * correction). Update cputime but don't calculate usage this
                 * cycle.
                 */
                p->ppid = tmp_process->ppid;
                p->cputime = tmp_process->cputime;
                p->cpu_usage = -1;
                continue;
            }
            if (dt < MIN_DT) {
                /* Time delta too small for accurate CPU measurement; keep
                 * cputime unchanged so the next valid update accumulates
                 * the full delta over the interval. Updating ppid is safe
                 * here because it is independent of timing accuracy: the
                 * parent PID is a current kernel value and does not
                 * participate in any time-based delta computation. */
                p->ppid = tmp_process->ppid;
                continue;
            }
            /*
             * Calculate CPU usage sample:
             * sample = (delta_cputime / delta_walltime)
             * This represents the fraction of one CPU core used.
             */
            sample = (tmp_process->cputime - p->cputime) / dt;
            /* Cap sample at total CPU capacity (shouldn't exceed N cores) */
            sample = MIN(sample, (double)ncpu);
            if (p->cpu_usage < 0) {
                /* First valid measurement: initialize directly */
                p->cpu_usage = sample;
            } else {
                /*
                 * Apply exponential moving average for smooth tracking:
                 * new = (1-alpha)*old + alpha*sample
                 * This reduces noise while remaining responsive to changes.
                 */
                p->cpu_usage = (1.0 - ALPHA) * p->cpu_usage + ALPHA * sample;
            }
            /* Update stored CPU time and parent PID for next delta calculation
             */
            p->ppid = tmp_process->ppid;
            p->cputime = tmp_process->cputime;
        }
    }
    free(tmp_process);
    if (close_process_iterator(&it) != 0) {
        exit(EXIT_FAILURE);
    }

    /* Remove hash table entries for processes that are no longer running */
    process_table_remove_stale(pgroup->proctable, pgroup->proclist);

    /*
     * Update timestamp only if sufficient time passed for CPU calculation
     * or if time moved backwards (to establish new baseline).
     */
    if (dt < 0 || dt >= MIN_DT) {
        pgroup->last_update = now;
    }
}

/**
 * @brief Calculate aggregate CPU usage across all processes in the group
 * @param pgroup Pointer to the process_group structure to query
 * @return Sum of CPU usage values for all processes with known usage, or
 *         -1.0 if no processes have valid CPU measurements yet or if
 *         pgroup is NULL
 *
 * CPU usage is expressed as a fraction of total system CPU capacity:
 * - 0.0 = idle
 * - 1.0 = fully utilizing one CPU core
 * - N = fully utilizing N CPU cores (on multi-core systems)
 *
 * The function:
 * 1. Iterates through all processes in proclist
 * 2. Sums cpu_usage for processes with valid measurements (cpu_usage >= 0)
 * 3. Returns -1 if all processes have unknown usage (first update cycle)
 *
 * @note Returns -1 rather than 0 to distinguish "no usage" from "unknown"
 * @note Thread-safe if pgroup is not being modified concurrently
 * @note Safe to call with NULL pgroup (returns -1)
 */
double get_process_group_cpu_usage(const struct process_group *pgroup) {
    const struct list_node *node;
    double cpu_usage = -1;
    if (pgroup == NULL) {
        return -1;
    }
    for (node = first_node(pgroup->proclist); node != NULL; node = node->next) {
        const struct process *p = (const struct process *)node->data;
        /* Skip NULL-data nodes (should not occur but defensive) */
        if (p == NULL) {
            continue;
        }
        /* Skip processes without valid CPU measurements yet */
        if (p->cpu_usage < 0) {
            continue;
        }
        /* Initialize sum on first valid process */
        if (cpu_usage < 0) {
            cpu_usage = 0;
        }
        cpu_usage += p->cpu_usage;
    }
    return cpu_usage;
}
