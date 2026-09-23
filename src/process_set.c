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

#include <errno.h>
#include <float.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

/**
 * @brief Compare two double values for approximate equality within DBL_EPSILON
 * @param a First double value
 * @param b Second double value
 * @return 1 if the values are approximately equal, 0 otherwise
 */
static int start_time_matches(double a, double b) {
    return a - b >= -DBL_EPSILON && a - b <= DBL_EPSILON;
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
    if (init_process_table(proc_set->proc_table, PROCESS_TABLE_HASHSIZE) != 0) {
        /*
         * Bucket allocation failed: drop the table structure itself and
         * report the error, so the caller can still resume a suspended
         * group instead of being killed by an exit() here.
         */
        free(proc_set->proc_table);
        proc_set->proc_table = NULL;
        return -1;
    }
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

int record_stopped_pid(struct process_set *proc_set, pid_t pid,
                       double start_time) {
    struct stopped_pid_record *rec;
    struct list_node *node;

    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return -1;
    }
    /* Update in place when this PID is already recorded (see above). */
    for (node = first_list_node(proc_set->stopped_pids); node != NULL;
         node = node->next) {
        rec = (struct stopped_pid_record *)node->data;
        if (rec != NULL && rec->pid == pid) {
            rec->start_time = start_time;
            return 0;
        }
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
        return -1;
    }
    rec->pid = pid;
    rec->start_time = start_time;
    if (add_list_elem(proc_set->stopped_pids, rec) == NULL) {
        /*
         * The list node could not be allocated either, so this suspension
         * is just as unrecorded as the malloc failure above: release the
         * orphaned record and undo the suspension now.  Leaving it
         * suspended would strand it silently once the member leaves the
         * group, because nothing would be left to resume it.
         */
        free(rec);
        kill(pid, SIGCONT);
        return -1;
    }
    return 0;
}

/*
 * Declared here because resume_stopped_pids() has to report a failed resume
 * the same way process_set_send_signal() reports a failed signal inside the
 * group; that definition sits further down with its only other caller.
 * Every PID in the stopped list was successfully suspended by this group,
 * so a failed resume really may leave it stopped.
 */
static void warn_signal_failure(int sig, pid_t pid, int err, int verbose,
                                int may_remain_stopped);

int resume_stopped_pids(struct process_set *proc_set) {
    const struct list_node *node;
    int failed = 0;
    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return 0;
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
             * A failure with ESRCH means the process is already gone, so
             * there is no suspension left to undo and nothing to report;
             * every other errno keeps the always-report policy and counts
             * as stranded so the caller can fail the shutdown.
             */
            if (kill(pid, SIGCONT) != 0) {
                int err = errno;
                warn_signal_failure(SIGCONT, pid, err, 0, 1);
                if (err != ESRCH) {
                    failed++;
                }
            }
        }
    }
    destroy_list(proc_set->stopped_pids);
    return failed;
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
 * @param now       Current timestamp of this update cycle
 * @param ncpu      Number of available CPU cores (used to cap the sample)
 *
 * The sampling interval is measured from proc->cpu_time_ts, the moment
 * the stored cpu_time baseline was recorded, not from the process set's
 * last_update: a member discovered mid-cycle has a later baseline, and
 * dividing its delta by the longer global interval would understate its
 * usage.  For a member present since the previous cycle the two
 * intervals coincide, because both baselines advance on every valid
 * update.
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
                                          const struct timespec *now,
                                          int ncpu) {
    double elapsed_ms;
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
         * the throttled group.  The start time is the authoritative
         * identity, so use it as the second detection signal.  Reset all
         * historical data.  The memcpy also clears suspended_by_us: the
         * replacement process was never suspended by this group, and the
         * iterator snapshots it is copied from always carry 0 there because
         * the platform get_next_process() implementations zero the whole
         * structure.  The CPU baseline timestamp is re-stamped below
         * because a snapshot always carries {0, 0}.
         */
        memcpy(proc, scan_proc, sizeof(*proc));
        /* Mark CPU usage as unknown for new process */
        proc->cpu_usage = -1;
        proc->cpu_time_ts = *now;
        return;
    }
    /*
     * In all non-reuse cases the parent PID is always updated to the
     * current value; it is independent of timing accuracy.
     */
    proc->ppid = scan_proc->ppid;
    elapsed_ms = timediff_in_ms(now, &proc->cpu_time_ts);
    if (elapsed_ms < 0) {
        /*
         * Time moved backwards (system clock adjustment, NTP
         * correction). Update cpu_time but don't calculate usage this
         * cycle.
         */
        proc->cpu_time = scan_proc->cpu_time;
        proc->cpu_time_ts = *now;
        proc->cpu_usage = -1;
        return;
    }
    if (elapsed_ms < CPU_MIN_DELTA_MS) {
        /*
         * Time delta too small for accurate CPU measurement; keep
         * cpu_time and its timestamp unchanged so the next valid update
         * accumulates the full delta over the interval.
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
    proc->cpu_time_ts = *now;
}

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
        if (!start_time_matches(proc_set->target_start_time,
                                UNKNOWN_START_TIME) &&
            !start_time_matches(scan_proc->start_time, UNKNOWN_START_TIME) &&
            scan_proc->pid == proc_set->target_pid &&
            !start_time_matches(scan_proc->start_time,
                                proc_set->target_start_time)) {
            target_replaced = 1;
            break;
        }
        proc = find_in_process_table(proc_set->proc_table, scan_proc->pid);
        if (proc == NULL) {
            /* New process detected: add to hashtable and list */
            proc = process_dup(scan_proc);
            if (proc == NULL) {
                /*
                 * Out of memory: abandon this scan cycle.  Returning -1
                 * makes limit_process() break its loop and run the cleanup
                 * that resumes every still-stopped member, so a failure
                 * here cannot leave a process suspended.
                 */
                alloc_failed = 1;
                break;
            }
            /* Mark CPU usage as unknown until we have a time delta */
            proc->cpu_usage = -1;
            /*
             * Stamp the CPU baseline with the moment it was taken:
             * the snapshot carries {0, 0} because the iterator zeroes the
             * whole structure, so this must be set explicitly.  A member
             * discovered later in the cycle gets a later baseline, and
             * its next sample is divided by exactly that interval.
             */
            proc->cpu_time_ts = now;
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
             * would be signalled and accounted for twice.  Only
             * re-add when it is not already in the list.
             *
             * The CPU accounting must equally run at most once per PID and
             * cycle: the first occurrence already refreshed cpu_time, so a
             * repeated snapshot would compute a bogus near-zero sample and
             * drag the EMA -- and with it the group's whole usage estimate
             * -- down.  Everything below is inside the same
             * first-occurrence branch for that reason.
             */
            if (find_process_in_list_by_pid(proc_set->proc_list, proc->pid) ==
                NULL) {
                if (add_list_elem(proc_set->proc_list, proc) == NULL) {
                    /*
                     * The member could not be linked into this cycle's
                     * view.  Besides missing this cycle's SIGSTOP/
                     * SIGCONT, a member absent from proc_list would be
                     * treated as exited by
                     * remove_stale_from_process_table() at the end of the
                     * cycle and lose its CPU history.  Abort the cycle
                     * like the other allocation failures.  Unlike the
                     * new-member path above, the record must NOT be
                     * freed here: it is still owned by proc_table, which
                     * keeps it for the next cycle's baseline comparison.
                     */
                    alloc_failed = 1;
                    break;
                }
                update_existing_process_entry(proc, scan_proc, &now, ncpu);
            }
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
         * An allocation in the scan loop failed.  limit_process() sees the
         * -1, breaks its loop and resumes the group through its own cleanup
         * path, so the failure cannot strand a stopped process.
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

int process_set_is_empty(const struct process_set *proc_set) {
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return 1;
    }
    return is_empty_list(proc_set->proc_list);
}

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
 * @param verbose Retained for the callers' API; whether to report at all is
 *                already decided by the caller's per-member gate, so this
 *                argument does not change what is printed
 * @param may_remain_stopped Non-zero when the failed signal was a SIGCONT
 *                           that would have undone a suspension this group
 *                           recorded, so the member may stay stopped forever
 *
 * Prints a single line: the recovery hint when the member may be stranded,
 * the ordinary "cannot send signal" line otherwise.  A failed SIGCONT for a
 * process that no longer exists is not reported, because there is no
 * suspension left to undo.  Reporting happens even without -v, since it means
 * the requested limit cannot be enforced on that process and the user has to
 * be told.  Deciding which failures are worth reporting at all is
 * classify_signal_failure()'s job, and the per-member gating that keeps a
 * retried failure from flooding the terminal belongs to the caller.
 */
static void warn_signal_failure(int sig, pid_t pid, int err, int verbose,
                                int may_remain_stopped) {
    (void)verbose;
    /* Nothing is suspended for a process that no longer exists. */
    if (sig == SIGCONT && err == ESRCH) {
        return;
    }
    /* Only an unresumed member needs the hint that names its PID. */
    if (sig == SIGCONT && may_remain_stopped) {
        fprintf(
            stderr,
            "Warning: cannot resume PID %ld with SIGCONT: %s\n         It may remain stopped; run 'kill -CONT %ld' to recover.\n",
            (long)pid, strerror(err), (long)pid);
        return;
    }

    fprintf(
        stderr,
        "Warning: cannot send signal %d to PID %ld: %s\n         (process stays tracked but cannot be limited)\n",
        sig, (long)pid, strerror(err));
}

/**
 * @def SIGNAL_FAILURE_BENIGN
 * @brief classify_signal_failure() result: the member has been running all
 *        along, so the failed signal stranded nothing
 */
#define SIGNAL_FAILURE_BENIGN 0

/**
 * @def SIGNAL_FAILURE_SEVERE
 * @brief classify_signal_failure() result: the failure counts and may have
 *        left the member stopped
 */
#define SIGNAL_FAILURE_SEVERE 1

/**
 * @brief Decide what kind of failure one undelivered signal is
 * @param sig The signal whose delivery failed
 * @param proc The member the signal was meant for
 * @param gate Out: the member flag that gates this kind of failure, so the
 *        caller can report once per episode without knowing which of the
 *        four warning flags applies here
 * @param may_remain_stopped Out: non-zero when this failure can have left
 *        the member stopped
 * @return SIGNAL_FAILURE_BENIGN or SIGNAL_FAILURE_SEVERE
 *
 * A failed SIGCONT for a member this group never suspended is benign: that
 * member was never stopped by us and has been running all along, so nothing
 * is stranded and the failure does not count towards the caller's result.
 * Every other failure is severe - it counts, and it may carry the recovery
 * hint that names the PID to resume by hand.
 *
 * Each kind gates on its own flag instead of sharing one, because the benign
 * and severe episodes of the same member vary independently: a member whose
 * SIGCONT failed before this group ever suspended it has already consumed the
 * benign gate, and reusing that gate for its severe episode would swallow the
 * one message that says which PID to recover.
 */
static int classify_signal_failure(int sig, struct process *proc, int **gate,
                                   int *may_remain_stopped) {
    if (sig == SIGCONT && !proc->suspended_by_us) {
        *gate = &proc->cont_warned;
        *may_remain_stopped = 0;
        return SIGNAL_FAILURE_BENIGN;
    }
    if (sig == SIGCONT) {
        *gate = &proc->resume_warned;
    } else {
        *gate = &proc->stop_warned;
    }
    *may_remain_stopped = proc->suspended_by_us;
    return SIGNAL_FAILURE_SEVERE;
}

int process_set_send_signal(struct process_set *proc_set, int sig,
                            int verbose) {
    struct list_node *node;
    int failed = 0;

    /*
     * Resume recorded PIDs before the guard below: those processes have
     * already left the group, so a group whose list is gone still owes
     * them a SIGCONT, and this is their last chance at one.  A failure
     * there is counted in failed, exactly like a failed resume of a
     * current member: both may strand a process this group suspended.
     */
    if (sig == SIGCONT) {
        failed = resume_stopped_pids(proc_set);
    }
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        /*
         * Return what the deferred round reported instead of a bare 0:
         * the group list may be gone while recorded suspensions remain
         *, and that failure must not be dropped here.
         */
        return failed;
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
            /* Chosen by classify_signal_failure() below. */
            int *gate;
            int may_remain_stopped;
            int kind;
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
                 * Keep tracking it: dropping it here would silently end
                 * the limit for that process, which would then keep
                 * running past the requested budget with nothing in the
                 * output explaining why.  It stays in the set instead, so
                 * its CPU time is still accounted for and the remaining
                 * members are still held to the budget.
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
                 * every failure would flood the terminal.  Report
                 * once per failure episode and only re-report once a
                 * successful delivery clears the flag.
                 *
                 * A failed SIGCONT only counts -- and only claims "may
                 * remain stopped" -- for a member this group suspended and
                 * cannot resume.  For a member never suspended
                 * (its signals were never deliverable) the failed SIGCONT
                 * is an ordinary failure: warn through the same once-per-
                 * episode gate, but do not report the group as having left
                 * anything suspended, because that member has been running
                 * all along.
                 */
                kind = classify_signal_failure(sig, proc, &gate,
                                               &may_remain_stopped);
                /*
                 * Report once per failure episode: gate is whichever flag
                 * the classifier picked for this kind of failure, and a
                 * later successful delivery clears it.
                 */
                if (!*gate) {
                    warn_signal_failure(sig, pid, saved_errno, verbose,
                                        may_remain_stopped);
                }
                *gate = 1;
                /*
                 * Only a severe failure counts towards what this call
                 * reports, and only it can have left a member stranded.
                 */
                if (kind == SIGNAL_FAILURE_SEVERE) {
                    failed++;
                    if (sig == SIGCONT && proc->suspended_by_us) {
                        /*
                         * Re-record the suspension this group still owes
                         * an undo for.  The round opened by destroying
                         * every record, so this member is stopped with no
                         * record left: if it now leaves the group -- a
                         * descendant re-parented away when its ancestor
                         * exits is the usual case -- the table entry that
                         * keeps it visible disappears too, and nothing
                         * would ever resume it again.  Recording it
                         * again cannot duplicate an entry: even when a
                         * stale entry survives into the next SIGSTOP round,
                         * record_stopped_pid() folds the second recording
                         * into the first, so the list stays free of
                         * duplicates; the next SIGCONT round retries whether
                         * it is still a member by then or has already left.
                         *
                         * Only for a member whose suspension is on the
                         * books: suspended_by_us is still set here because
                         * only a successful SIGCONT clears it.  A failure
                         * to allocate the record resumes the member right
                         * away, so ignoring the result is safe.
                         */
                        (void)record_stopped_pid(proc_set, pid,
                                                 proc->start_time);
                    }
                }
            }
        } else if (sig == SIGSTOP) {
            /*
             * Track the suspension so it can always be undone, and mark the
             * member as suspended by this group only when the record really
             * exists: if record_stopped_pid() could not
             * record it, that function has already resumed the member, so
             * the flag must not claim a suspension that was taken back.
             */
            if (record_stopped_pid(proc_set, pid, proc->start_time) == 0) {
                proc->suspended_by_us = 1;
            }
            /*
             * A successful delivery ends this signal's failure episode,
             * exactly as the SIGCONT branch clears cont_warned: without
             * this, one early SIGSTOP failure silenced every later one
             * for the rest of the session, hiding a limit that stopped
             * being enforceable.
             */
            proc->stop_warned = 0;
        } else {
            /* SIGCONT delivered: clear the warnable state so a later
             * failure re-reports instead of going unnoticed.  The
             * suspension this flag tracks is undone as well. */
            proc->cont_warned = 0;
            proc->resume_warned = 0;
            proc->suspended_by_us = 0;
        }
        node = next_node;
    }
    return failed;
}

#ifdef CPULIMIT_TEST_BUILD
/*
 * Test accessor for record_stopped_pid()'s de-duplication.  A process_set
 * that only carries a stopped_pids list is built locally, the same PID is
 * recorded twice, and the surviving entries are counted.  With the fix a
 * re-recording folds into the existing entry, so the count is 1 and its start
 * time is the latest one; the production path relies on exactly that so the
 * SIGSTOP round cannot double a member re-recorded after a failed SIGCONT.
 * Defined only in the test build so the production object stays free of test
 * code.
 */
int cpulimit_test_record_stopped_pid_dedup(void) {
    struct process_set proc_set;
    struct list_node *node;
    const struct stopped_pid_record *rec;
    int count;
    double seen_start;

    memset(&proc_set, 0, sizeof(proc_set));
    proc_set.stopped_pids = (struct list *)malloc(sizeof(struct list));
    if (proc_set.stopped_pids == NULL) {
        return -1;
    }
    init_list(proc_set.stopped_pids);

    (void)record_stopped_pid(&proc_set, (pid_t)4242, 100.0);
    (void)record_stopped_pid(&proc_set, (pid_t)4242, 200.0);

    count = 0;
    seen_start = -1.0;
    for (node = first_list_node(proc_set.stopped_pids); node != NULL;
         node = node->next) {
        rec = (const struct stopped_pid_record *)node->data;
        if (rec != NULL && rec->pid == (pid_t)4242) {
            count++;
            seen_start = rec->start_time;
        }
    }

    /* destroy_list() frees each node's record as well. */
    destroy_list(proc_set.stopped_pids);
    free(proc_set.stopped_pids);
    /* start_time_matches() avoids a bare float == (it is exact here, but the
     * compiler still warns about -Wfloat-equal). */
    return (count == 1 && start_time_matches(seen_start, 200.0)) ? 0 : -1;
}
#endif
