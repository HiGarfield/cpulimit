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

#include "process_set.h"

#include "cpu_count.h"
#include "list.h"
#include "process_iterator.h"
#include "process_table.h"
#include "time_util.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Bit-exact double equality, used only for the UNKNOWN_START_TIME sentinel and
 * for detecting PID reuse via start-time identity.  A plain `==' on doubles
 * trips -Wfloat-equal even though these values are exact sentinels, so compare
 * the raw bytes instead.  Returns 1 when the two doubles are bit-identical.
 */
static int start_time_matches(double a, double b) {
    return memcmp(&a, &b, sizeof(double)) == 0;
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
 * @brief Initialize a process set for monitoring and CPU limiting
 * @param proc_set Pointer to uninitialized process_set structure to set up
 * @param target_pid PID of the primary process to monitor
 * @param include_children Non-zero to monitor descendants, zero for target only
 * @return 0 on success, -1 on error
 *
 * This function:
 * 1. Allocates and initializes the process hashtable (PROCESS_TABLE_HASHSIZE
 *    buckets)
 * 2. Allocates and initializes the process list
 * 3. Records the current time as baseline for CPU calculations
 * 4. Performs initial update to populate the process list
 *
 * @note Returns -1 immediately if proc_set is NULL
 * @note Returns -1 on memory allocation, timing, or initial scan errors;
 *       partially allocated resources are released before returning
 * @note After successful return, proc_set is fully initialized and ready
 *       for use
 */
int init_process_set(struct process_set *proc_set, pid_t target_pid,
                     int include_children) {
    if (proc_set == NULL) {
        return -1;
    }
    memset(proc_set, 0, sizeof(*proc_set));
    /* Allocate and initialize hashtable for fast process lookup by PID */
    proc_set->proc_table =
        (struct process_table *)malloc(sizeof(struct process_table));
    if (proc_set->proc_table == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        return -1;
    }
    init_process_table(proc_set->proc_table, PROCESS_TABLE_HASHSIZE);
    proc_set->target_pid = target_pid;
    proc_set->include_children = include_children;

    /* Allocate and initialize linked list for process iteration */
    proc_set->proc_list = (struct list *)malloc(sizeof(struct list));
    if (proc_set->proc_list == NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        close_process_set(proc_set);
        return -1;
    }
    init_list(proc_set->proc_list);

    /* Allocate and initialize the list of PIDs suspended by this group */
    proc_set->stopped_pids = (struct list *)malloc(sizeof(struct list));
    if (proc_set->stopped_pids == NULL) {
        fprintf(stderr,
                "Memory allocation failed for the suspended process list\n");
        close_process_set(proc_set);
        return -1;
    }
    init_list(proc_set->stopped_pids);

    /* Record baseline timestamp for CPU usage calculation */
    if (get_current_time(&proc_set->last_update) != 0) {
        perror("get_current_time");
        close_process_set(proc_set);
        return -1;
    }
    /* No reuse baseline yet: the initial scan must not reject the target. */
    proc_set->target_start_time = UNKNOWN_START_TIME;
    /* Perform initial scan to populate process list */
    if (update_process_set(proc_set) != 0) {
        fprintf(stderr, "Failed to perform initial process group scan\n");
        close_process_set(proc_set);
        return -1;
    }
    /*
     * Remember when the target started. Without this, a target that exits
     * and has its PID recycled mid-session looks identical to the target:
     * the group would keep suspending whatever now occupies that PID, for
     * as long as cpulimit runs.
     */
    if (target_pid > 0) {
        const struct process *target =
            find_in_process_table(proc_set->proc_table, target_pid);
        if (target != NULL) {
            proc_set->target_start_time = target->start_time;
        }
    }
    return 0;
}

/**
 * @brief Release all resources associated with a process set
 * @param proc_set Pointer to the process_set structure to clean up
 * @return 0 on success (always succeeds)
 *
 * This function:
 * 1. Clears and frees the process list
 * 2. Destroys and frees the process hashtable
 * 3. Sets pointers to NULL and zeros numeric fields for safety
 *
 * @note Safe to call with NULL proc_set (returns 0 immediately)
 * @note Safe to call even if proc_set is partially initialized (NULLs are
 *       handled)
 * @note Does not send any signals to processes; they continue running
 * @note After return, proc_set fields should not be accessed without
 *       re-initialization
 */
int close_process_set(struct process_set *proc_set) {
    if (proc_set == NULL) {
        return 0;
    }
    if (proc_set->proc_list != NULL) {
        /*
         * Use clear_list (not destroy_list) because the data pointers in
         * proc_list are the same process structs stored in proc_table.
         * destroy_process_table below will free all data exactly once.
         * Using destroy_list here would double-free the process structs.
         */
        clear_list(proc_set->proc_list);
        free(proc_set->proc_list);
        proc_set->proc_list = NULL;
    }

    if (proc_set->stopped_pids != NULL) {
        /*
         * Each element is a heap-allocated stopped_pid_record owned by this
         * list, so destroy_list() (not clear_list()) is required to release
         * the elements together with their nodes.
         */
        destroy_list(proc_set->stopped_pids);
        free(proc_set->stopped_pids);
        proc_set->stopped_pids = NULL;
    }

    if (proc_set->proc_table != NULL) {
        destroy_process_table(proc_set->proc_table);
        free(proc_set->proc_table);
        proc_set->proc_table = NULL;
    }

    /* Zero out remaining fields to prevent stale data after close */
    memset(proc_set, 0, sizeof(*proc_set));

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
 * @note Returns NULL if memory allocation fails, so the caller can abort
 *       the scan and let limit_process() resume the group cleanly
 */
static struct process *process_dup(const struct process *proc) {
    struct process *new_proc;
    new_proc = (struct process *)malloc(sizeof(struct process));
    if (new_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        return NULL;
    }
    /* Copy via memcpy: avoids generating a large stack temporary for a
       by-value struct assignment (struct process is ~4 KiB). */
    memcpy(new_proc, proc, sizeof(*new_proc));
    return new_proc;
}

/*
 * A suspension record: the PID that was stopped, plus the start time it had
 * at that moment. The start time lets resume_stopped_pids() confirm the PID
 * still belongs to the same process before sending the undoing SIGCONT, so a
 * recycled PID does not receive a spurious resume meant for its predecessor.
 */
struct stopped_pid_record {
    pid_t pid;
    double start_time;
};

/**
 * @brief Record that a member of the group has just been suspended
 * @param proc_set Pointer to the process set structure
 * @param pid PID that was successfully sent SIGSTOP
 * @param start_time Start time of pid at suspension, from get_process_start_time()
 *
 * proc_list is rebuilt from scratch by update_process_set(), so a process
 * can cease to be a member of the group while it is still suspended: a
 * descendant, for instance, is re-parented away when its monitored ancestor
 * exits, and is_child_of() then no longer matches it.  Recording the PID
 * here keeps the suspension undoable after the process has left proc_list.
 */
void record_stopped_pid(struct process_set *proc_set, pid_t pid,
                        double start_time) {
    struct stopped_pid_record *rec;
    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return;
    }
    rec = (struct stopped_pid_record *)malloc(sizeof(*rec));
    if (rec == NULL) {
        /*
         * Nothing can record this suspension, so nothing would be able to
         * undo it later: undo it now. Leaving the process suspended would
         * be worse than letting it run for the rest of this cycle, and it
         * stays a group member, so limiting resumes from the next one.
         */
        kill(pid, SIGCONT);
        return;
    }
    rec->pid = pid;
    rec->start_time = start_time;
    add_list_elem(proc_set->stopped_pids, rec);
}

/*
 * Declared here because resume_stopped_pids() has to report a failed resume
 * the same way process_set_send_signal() reports a failed signal inside the
 * group; that definition sits further down with its only other caller.
 */
static void warn_signal_failure(int sig, pid_t pid, int err, int verbose);

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
 */
void resume_stopped_pids(struct process_set *proc_set) {
    const struct list_node *node;
    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return;
    }
    for (node = first_list_node(proc_set->stopped_pids); node != NULL;
         node = node->next) {
        const struct stopped_pid_record *rec =
            (const struct stopped_pid_record *)node->data;
        pid_t pid = rec->pid;
        /*
         * A process that is still a group member is resumed by the
         * regular SIGCONT round that walks proc_list, so signalling it
         * here as well would deliver a second, redundant SIGCONT. Only
         * the processes that have left the group need one here.
         */
        if (find_process_in_list_by_pid(proc_set->proc_list, pid) == NULL) {
            /*
             * The PID may have been recycled since it was suspended. If we
             * recorded its start time, re-query it now: a different process
             * occupying the recycled PID must not receive the resume that
             * was meant for its predecessor. When the recorded start time is
             * unknown we cannot tell, so we fall back to resuming it.
             */
            if (!start_time_matches(rec->start_time, UNKNOWN_START_TIME)) {
                double current = get_process_start_time(pid);
                if (!start_time_matches(current, UNKNOWN_START_TIME) &&
                    !start_time_matches(current, rec->start_time)) {
                    /*
                     * PID reused: the original process is gone and the new
                     * one was never suspended by us, so skip the SIGCONT.
                     */
                    continue;
                }
            }
            /*
             * Last chance for this process: the record is dropped below,
             * so a failure here leaves it suspended with nothing left to
             * retry it. Say so instead of letting it stop silently.
             */
            if (kill(pid, SIGCONT) != 0) {
                int err = errno;
                warn_signal_failure(SIGCONT, pid, err, 0);
            }
        }
    }
    destroy_list(proc_set->stopped_pids);
}

void forget_stopped_pid(struct process_set *proc_set, pid_t pid) {
    struct list_node *node, *next_node;
    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return;
    }
    for (node = first_list_node(proc_set->stopped_pids); node != NULL;
         node = next_node) {
        const struct stopped_pid_record *rec =
            (const struct stopped_pid_record *)node->data;
        next_node = node->next;
        if (rec->pid != pid) {
            continue;
        }
        /*
         * Each element is a heap-allocated stopped_pid_record owned by this
         * list, so it has to be released before its node is unlinked:
         * delete_list_node() only frees the node.
         */
        free(node->data);
        delete_list_node(proc_set->stopped_pids, node);
    }
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
 * @brief Update the CPU usage of an existing tracked process entry
 * @param proc      The stored process entry to update (modified in place)
 * @param scan_proc Fresh snapshot of the same process from the iterator
 * @param elapsed_ms Milliseconds elapsed since the last update cycle
 * @param ncpu      Number of available CPU cores (used to cap the sample)
 *
 * Handles four mutually exclusive cases:
 * - PID reuse (scan_proc->cpu_time < proc->cpu_time, or the start time
 *   differs despite the same PID): resets all fields.
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
    if (scan_proc->cpu_time < proc->cpu_time ||
        (!start_time_matches(proc->start_time, UNKNOWN_START_TIME) &&
         !start_time_matches(scan_proc->start_time, UNKNOWN_START_TIME) &&
         !start_time_matches(proc->start_time, scan_proc->start_time))) {
        /*
         * CPU time decreased, or the start time changed while the PID is
         * unchanged: the PID has been recycled for a new process.  A fresh
         * process after a reuse often has a *higher* CPU time than the old
         * one (especially when the old process had barely started), so the
         * cpu_time-only check would miss it and the new process would be
         * misattributed to the old entry -- keeping an innocent process in
         * the throttled group (BUG-004).  The start time is the authoritative
         * identity, so use it as the second detection signal.  Reset all
         * historical data.
         */
        memcpy(proc, scan_proc, sizeof(*proc));
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
        /*
         * Time delta too small for accurate CPU measurement; keep
         * cpu_time unchanged so the next valid update accumulates
         * the full delta over the interval.
         */
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
int update_process_set(struct process_set *proc_set) {
    struct process_iterator iter;
    struct process *scan_proc;
    struct process_filter filter;
    struct timespec now;
    double elapsed_ms;
    int ncpu, close_ret;
    pid_t self_pid;
    int target_replaced;
    int alloc_failed;
    if (proc_set == NULL || proc_set->proc_list == NULL ||
        proc_set->proc_table == NULL) {
        return 0;
    }
    ncpu = get_ncpu(); /* get_ncpu() caches its result across calls */
    self_pid = getpid();
    target_replaced = 0;
    alloc_failed = 0;

    /* Get current timestamp for delta calculation */
    if (get_current_time(&now) != 0) {
        perror("get_current_time");
        return -1;
    }
    scan_proc = (struct process *)malloc(sizeof(struct process));
    if (scan_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for scan_proc\n");
        return -1;
    }
    /* Calculate elapsed time since last update (milliseconds) */
    elapsed_ms = timediff_in_ms(&now, &proc_set->last_update);

    /* Configure iterator to scan target process and optionally descendants */
    filter.pid = proc_set->target_pid;
    filter.include_children = proc_set->include_children;
    filter.read_cmd = 0;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(scan_proc);
        return -1;
    }

    /*
     * Clear process list (will be rebuilt from scratch).  Only the list
     * nodes are released: the records they pointed at belong to
     * proc_table, which must keep them so that the next cycle can still
     * compare cpu_time against the previous sample.  This is the point of
     * the ownership contract documented on struct process_set.
     */
    clear_list(proc_set->proc_list);

    /* Scan currently running processes and update tracking data */
    while (get_next_process(&iter, scan_proc) != -1) {
        struct process *proc;
        /*
         * Never track cpulimit itself. With --include-children the target
         * may be an ancestor of this process, and is_child_of() then
         * reports it as a group member like any other descendant.
         * Suspending it would stop the limiter before it can reach the
         * cleanup that resumes the group, and SIGSTOP can be neither
         * caught nor blocked, so the whole group would stay suspended.
         * Its own CPU time must not count against the target's budget
         * either.
         */
        if (scan_proc->pid == self_pid) {
            continue;
        }
        /*
         * PID reuse of the target: this group was built around a process
         * that started at target_start_time, and here is a different
         * process wearing the same PID. Nothing in this scan is the target
         * any more, so stop and leave the group empty -- limit_process()
         * then returns, and the caller either exits or, with -e, searches
         * for the target by name again.
         *
         * Only the target is checked. A recycled descendant PID is not a
         * hazard on its own: is_child_of() decides membership from the
         * live parent chain, so the replacement rarely matches at all.
         */
        if (!start_time_matches(proc_set->target_start_time, UNKNOWN_START_TIME) &&
            !start_time_matches(scan_proc->start_time, UNKNOWN_START_TIME) &&
            scan_proc->pid == proc_set->target_pid &&
            !start_time_matches(scan_proc->start_time, proc_set->target_start_time)) {
            target_replaced = 1;
            break;
        }
        proc = find_in_process_table(proc_set->proc_table, scan_proc->pid);
        if (proc == NULL) {
            /* New process detected: add to hashtable and list */
            proc = process_dup(scan_proc);
            if (proc == NULL) {
                /*
                 * Out of memory: abandon this scan cycle. Returning -1
                 * makes limit_process() break the limiting loop and run
                 * its own cleanup, which resumes every still-stopped member
                 * (no atexit needed, so no exit() left in this path).
                 */
                alloc_failed = 1;
                break;
            }
            /* Mark CPU usage as unknown until we have a time delta */
            proc->cpu_usage = -1;
            if (add_to_process_table(proc_set->proc_table, proc) != 0) {
                free(proc);
                alloc_failed = 1;
                break;
            }
            if (add_list_elem(proc_set->proc_list, proc) == NULL) {
                /*
                 * The duplicate is in the table but not yet in the list,
                 * so drop it from the table (which frees the data) and
                 * abort the scan rather than leaking the orphan entry.
                 */
                delete_from_process_table(proc_set->proc_table, proc->pid);
                alloc_failed = 1;
                break;
            }
        } else {
            /*
             * Existing process: re-add to the list for this cycle.  The list
             * was cleared at the top of the cycle, so a process that survived
             * from the previous cycle is legitimately absent and must be put
             * back.  A PID, however, can appear more than once within a single
             * iterator snapshot (a /proc race), and its first occurrence has
             * already added it -- adding it again would double-count it, so it
             * would be signalled and accounted for twice (BUG-073).  Only re-add
             * when it is not already in the list.
             */
            if (find_process_in_list_by_pid(proc_set->proc_list, proc->pid) == NULL) {
                add_list_elem(proc_set->proc_list, proc);
            }
            update_existing_process_entry(proc, scan_proc, elapsed_ms, ncpu);
        }
    }
    if (target_replaced) {
        /*
         * Drop whatever was gathered before the recycled PID showed up, so
         * the group reads as empty and purge the table entries below.
         */
        clear_list(proc_set->proc_list);
    }
    free(scan_proc);
    close_ret = close_process_iterator(&iter);
    if (close_ret != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }

    /*
     * Purge stale hash table entries unconditionally, even when the
     * iterator failed to close.  proc_list was rebuilt above and is
     * authoritative for this cycle, so returning early here would leave
     * entries for exited processes in proc_table forever, growing it
     * without bound during long runs (notably with --include-children).
     */
    remove_stale_from_process_table(proc_set->proc_table, proc_set->proc_list);

    if (close_ret != 0) {
        return -1;
    }
    if (alloc_failed) {
        /*
         * An allocation in the scan loop failed. limit_process() sees the
         * -1, breaks its loop and resumes the group through its own cleanup
         * path -- there is no exit() left here that could strand a stopped
         * process.
         */
        return -1;
    }

    /*
     * Update timestamp only if sufficient time passed for CPU calculation
     * or if time moved backwards (to establish new baseline).
     */
    if (elapsed_ms < 0 || elapsed_ms >= CPU_MIN_DELTA_MS) {
        proc_set->last_update = now;
    }
    return 0;
}

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
double get_process_set_cpu_usage(const struct process_set *proc_set) {
    const struct list_node *node;
    double cpu_usage = -1;
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return -1;
    }
    for (node = first_list_node(proc_set->proc_list); node != NULL;
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

/**
 * @brief Check whether the process set currently has no active members
 * @param proc_set Pointer to the process_set structure to query
 * @return Non-zero if proc_list is empty or proc_set is NULL
 */
int process_set_is_empty(const struct process_set *proc_set) {
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return 1;
    }
    return is_empty_list(proc_set->proc_list);
}

/**
 * @brief Return the number of active members in the process set
 * @param proc_set Pointer to the process_set structure to query
 * @return Number of nodes in proc_list, or 0 if proc_set is NULL
 */
size_t process_set_member_count(const struct process_set *proc_set) {
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return 0;
    }
    return proc_set->proc_list->count;
}

/**
 * @brief Report a signal that could not be delivered to a group member
 * @param sig Signal whose delivery failed
 * @param pid Process the signal could not be delivered to
 * @param err errno value captured at the point of failure
 * @param verbose If non-zero, report every occurrence instead of only the
 *                first one
 *
 * A process that cannot be signalled is retried on every control cycle,
 * so reporting every failure would flood the terminal; without
 * --verbose only the first one is reported.  The diagnostic is printed
 * even when not verbose because it means the requested limit cannot be
 * enforced on that process, which the user has to be told about.
 */
static void warn_signal_failure(int sig, pid_t pid, int err, int verbose) {
    static int warned = 0;

    if (sig == SIGCONT) {
        /*
         * A failed SIGCONT means a process this group suspended could not be
         * resumed, so it may stay stopped forever.  That is critical and must
         * not be swallowed by the once-only gate used for SIGSTOP, so it is
         * always reported with a recovery hint (BUG-049).
         */
        fprintf(stderr,
                "Warning: cannot resume PID %ld with SIGCONT: %s\n"
                "         It may remain stopped; run 'kill -CONT %ld' to recover.\n",
                (long)pid, strerror(err), (long)pid);
        return;
    }

    if (!verbose) {
        if (warned) {
            return;
        }
        warned = 1;
    }
    fprintf(stderr,
            "Warning: cannot send signal %d to PID %ld: %s\n"
            "         (process stays tracked but cannot be limited)\n",
            sig, (long)pid, strerror(err));
}

/**
 * @brief Send a signal to every active member of the process set
 * @param proc_set Pointer to the process set structure
 * @param sig Signal number to send (e.g., SIGSTOP, SIGCONT)
 * @param verbose If non-zero, print every signal delivery failure instead
 *                of only the first one
 *
 * Iterates through all processes in the group and sends the specified
 * signal.  A process that no longer exists (ESRCH) is removed from the
 * group and from the process table to avoid repeated errors.  A process
 * that still exists but could not be signalled (EPERM/EACCES, a seccomp
 * filter, ...) is kept: dropping it would silently end the limit for a
 * process the user asked to limit, while its CPU time still counts
 * against the group budget.  Such a failure is always reported, verbose
 * or not.
 *
 * Successful SIGSTOP delivery is recorded so that the suspension can
 * always be undone; SIGCONT additionally resumes processes that were
 * recorded earlier but have since left the group.
 *
 * @note Safe iteration: stores next node before potential deletion
 */
int process_set_send_signal(struct process_set *proc_set, int sig,
                            int verbose) {
    struct list_node *node;
    int failed = 0;

    /*
     * Resume recorded PIDs before the guard below: those processes have
     * already left the group, so a group whose list is gone still owes
     * them a SIGCONT, and this is their last chance at one.
     */
    if (sig == SIGCONT) {
        resume_stopped_pids(proc_set);
    }
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return 0;
    }

    node = first_list_node(proc_set->proc_list);
    while (node != NULL) {
        /* Save next pointer before potential node deletion */
        struct list_node *next_node = node->next;
        struct process *proc;
        pid_t pid;
        int kill_result;
        if (node->data == NULL) {
            /* Defensive: skip and remove any NULL-data nodes */
            delete_list_node(proc_set->proc_list, node);
            node = next_node;
            continue;
        }
        proc = (struct process *)node->data;
        pid = proc->pid;
        kill_result = kill(pid, sig);

        if (kill_result != 0) {
            /*
             * Signal delivery failed. Common reasons:
             * - ESRCH: Process no longer exists
             * - EPERM: Permission denied (rare in this context)
             *
             * kill() is non-blocking and cannot fail with EINTR, so any
             * failure here indicates a process that can no longer be
             * reliably controlled.
             * Save errno before any other calls that may clobber it.
             */
            int saved_errno = errno;
            if (saved_errno == ESRCH) {
                /*
                 * The process is gone. Drop the suspension record with
                 * it: resume_stopped_pids() treats a PID that is
                 * suspended but no longer a group member as something it
                 * still has to resume, and this process is about to stop
                 * being a member even though its signal just failed:
                 * there is nothing left to undo, and the PID may already
                 * have been recycled for an unrelated process.
                 */
                forget_stopped_pid(proc_set, pid);
                /* Remove the dead process from tracking */
                delete_list_node(proc_set->proc_list, node);
                delete_from_process_table(proc_set->proc_table, pid);
            } else {
                /*
                 * The process is still alive but refused the signal:
                 * EPERM/EACCES (a descendant that changed credentials or
                 * is owned by another user), a seccomp filter, and so on.
                 *
                 * Keep tracking it. Removing it here is what used to
                 * happen, and it silently ended the limit for that
                 * process: it kept running past the requested budget and
                 * nothing in the output explained why. It stays in the
                 * set instead, so its CPU time is still accounted for and
                 * the remaining members are still held to the budget.
                 *
                 * Consequence worth knowing: its usage keeps dragging
                 * work_ratio down, so if it alone exceeds the limit the
                 * controllable members are throttled harder. That is the
                 * honest outcome -- the group really is over budget -- and
                 * is preferable to ignoring the excess.
                 */
                /*
                 * Throttle the diagnostic: a member that can never be
                 * signalled is retried every control cycle, so reporting
                 * every failure would flood the terminal (BUG-058).  Report
                 * once per failure episode and only re-report once a
                 * successful delivery clears the flag.
                 */
                if ((sig == SIGCONT && !proc->cont_warned) ||
                    (sig != SIGCONT && !proc->stop_warned)) {
                    warn_signal_failure(sig, pid, saved_errno, verbose);
                }
                if (sig == SIGCONT) {
                    proc->cont_warned = 1;
                } else {
                    proc->stop_warned = 1;
                }
                failed++;
            }
        } else if (sig == SIGSTOP) {
            /* Track the suspension so it can always be undone */
            record_stopped_pid(proc_set, pid, proc->start_time);
        } else {
            /* SIGCONT delivered: clear the warnable state so a later
             * failure re-reports instead of going unnoticed. */
            proc->cont_warned = 0;
        }
        node = next_node;
    }
    return failed;
}
