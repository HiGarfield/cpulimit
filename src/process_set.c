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
 *
 * @param a First double value
 * @param b Second double value
 * @return 1 if the values are approximately equal, 0 otherwise
 *
 * Exported because PID-reuse detection spans two modules: the group compares
 * a member's recorded start time before resuming it, and the limiter compares
 * the target's before its closing SIGCONT. A second, independent comparison in
 * the limiter would be free to drift from this one, and a drifted definition
 * of "same process" is exactly the bug both call sites exist to prevent.
 */
int start_time_matches(double a, double b) {
    return a - b >= -DBL_EPSILON && a - b <= DBL_EPSILON;
}

/**
 * @def PROCESS_TABLE_HASHSIZE
 * @brief Number of hash buckets for the process hashtable (separate chaining).
 */
#define PROCESS_TABLE_HASHSIZE 2048

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
                     int include_children) {
    if (proc_set == NULL) {
        return -1;
    }
    memset(proc_set, 0, sizeof(*proc_set));
    proc_set->proc_table =
        (struct process_table *)malloc(sizeof(struct process_table));
    if (proc_set->proc_table == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        return -1;
    }
    if (init_process_table(proc_set->proc_table, PROCESS_TABLE_HASHSIZE) != 0) {
        free(proc_set->proc_table);
        proc_set->proc_table = NULL;
        return -1;
    }
    proc_set->target_pid = target_pid;
    proc_set->include_children = include_children;

    proc_set->proc_list = (struct list *)malloc(sizeof(struct list));
    if (proc_set->proc_list == NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        close_process_set(proc_set);
        return -1;
    }
    init_list(proc_set->proc_list);

    proc_set->stopped_pids = (struct list *)malloc(sizeof(struct list));
    if (proc_set->stopped_pids == NULL) {
        fprintf(stderr,
                "Memory allocation failed for the suspended process list\n");
        close_process_set(proc_set);
        return -1;
    }
    init_list(proc_set->stopped_pids);

    if (get_current_time(&proc_set->last_update) != 0) {
        perror("get_current_time");
        close_process_set(proc_set);
        return -1;
    }
    /* The initial scan must not reject the target as recycled. */
    proc_set->target_start_time = UNKNOWN_START_TIME;
    if (update_process_set(proc_set) != 0) {
        fprintf(stderr, "Failed to perform initial process group scan\n");
        close_process_set(proc_set);
        return -1;
    }
    /*
     * Remember when the target started, so a target that exits and has its
     * PID recycled mid-session is not mistaken for the same process.
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
 *
 * @param proc_set Pointer to the process_set structure to clean up
 * @return 0 on success (always succeeds)
 *
 * @note Safe to call with NULL proc_set or a partially initialized one
 */
int close_process_set(struct process_set *proc_set) {
    if (proc_set == NULL) {
        return 0;
    }
    if (proc_set->proc_list != NULL) {
        /*
         * clear_list, not destroy_list: the structs are owned by
         * proc_table and freed exactly once there.
         */
        clear_list(proc_set->proc_list);
        free(proc_set->proc_list);
        proc_set->proc_list = NULL;
    }

    if (proc_set->stopped_pids != NULL) {
        /* destroy_list: each element is a heap-allocated record. */
        destroy_list(proc_set->stopped_pids);
        free(proc_set->stopped_pids);
        proc_set->stopped_pids = NULL;
    }

    if (proc_set->proc_table != NULL) {
        destroy_process_table(proc_set->proc_table);
        free(proc_set->proc_table);
        proc_set->proc_table = NULL;
    }

    memset(proc_set, 0, sizeof(*proc_set));

    return 0;
}

/**
 * @brief Create a deep copy of a process structure
 *
 * @param proc Pointer to the source process structure to duplicate
 * @return Pointer to the copied process, or NULL on allocation failure
 *
 * @note Returns NULL on OOM so the caller can abort the scan and let
 *       limit_process() resume the group cleanly
 */
static struct process *process_dup(const struct process *proc) {
    struct process *new_proc;
    new_proc = (struct process *)malloc(sizeof(struct process));
    if (new_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        return NULL;
    }
    /* memcpy avoids a 4 KiB stack temporary from by-value assignment. */
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
                       double start_time) {
    struct stopped_pid_record *rec;
    struct list_node *node;

    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return -1;
    }

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
         * Cannot record it, so undo the suspension now: leaving it stopped
         * would be worse, and the member stays in the group.
         */
        kill(pid, SIGCONT);
        return -1;
    }
    rec->pid = pid;
    rec->start_time = start_time;
    if (add_list_elem(proc_set->stopped_pids, rec) == NULL) {
        /*
         * Node allocation also failed: free the orphaned record and undo
         * the suspension, or it is stranded once the member leaves.
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
         * Still-members are resumed by the regular round over proc_list;
         * signalling them here would be a redundant second SIGCONT.
         */
        if (find_process_in_list_by_pid(proc_set->proc_list, pid) == NULL) {
            /*
             * If recorded, re-check the start time: a recycled PID must not
             * receive the resume meant for its predecessor. Unknown start
             * time means we cannot tell, so fall back to resuming it.
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
        free(node->data);
        delete_list_node(proc_set->stopped_pids, node);
    }
}

/**
 * @def CPU_EMA_ALPHA
 * @brief Smoothing factor for the CPU-usage EMA (set to 0.4 for low lag).
 */
#define CPU_EMA_ALPHA 0.4

/**
 * @def CPU_MIN_DELTA_MS
 * @brief Minimum update interval (ms) for a valid CPU-usage sample.
 */
#define CPU_MIN_DELTA_MS 20

/**
 * @brief Update a tracked process entry with its latest scan sample
 *
 * @param proc      Stored entry to update in place
 * @param scan_proc Fresh sample of the same process from the iterator
 * @param now       Timestamp of this update cycle
 * @param ncpu      Number of CPU cores (used to cap the sample)
 *
 * The sample interval is measured from proc->cpu_time_ts, not the set's
 * last_update, so a member discovered mid-cycle keeps its own baseline.
 * A decreasing cpu_time or changed start time means the PID was recycled
 * (reset); a backward clock marks usage unknown; a too-short interval holds
 * cpu_time; otherwise a capped sample is EMA-smoothed.
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
         * CPU time dropped, or the start time changed while the PID is the
         * same: the PID was recycled. A fresh process often has higher CPU
         * time than the old one, so a cpu_time-only check would miss it and
         * misattribute the newcomer to the old entry. Start time is the
         * authoritative identity, so it is the second signal. Reset all
         * history; the snapshot's {0,0} baseline is re-stamped below.
         */
        memcpy(proc, scan_proc, sizeof(*proc));
        proc->cpu_usage = -1;
        proc->cpu_time_ts = *now;
        return;
    }
    proc->ppid = scan_proc->ppid;
    elapsed_ms = timediff_in_ms(now, &proc->cpu_time_ts);
    if (elapsed_ms < 0) {
        /* Clock went backwards: keep cpu_time but skip usage this cycle. */
        proc->cpu_time = scan_proc->cpu_time;
        proc->cpu_time_ts = *now;
        proc->cpu_usage = -1;
        return;
    }
    if (elapsed_ms < CPU_MIN_DELTA_MS) {
        /* Delta too small to measure: keep cpu_time for the next update. */
        return;
    }
    sample = (scan_proc->cpu_time - proc->cpu_time) / elapsed_ms;
    sample = MIN(sample, (double)ncpu);
    if (proc->cpu_usage < 0) {
        proc->cpu_usage = sample;
    } else {
        proc->cpu_usage =
            (1.0 - CPU_EMA_ALPHA) * proc->cpu_usage + CPU_EMA_ALPHA * sample;
    }
    proc->cpu_time = scan_proc->cpu_time;
    proc->cpu_time_ts = *now;
}

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

    if (get_current_time(&now) != 0) {
        perror("get_current_time");
        return -1;
    }
    scan_proc = (struct process *)malloc(sizeof(struct process));
    if (scan_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for scan_proc\n");
        return -1;
    }
    elapsed_ms = timediff_in_ms(&now, &proc_set->last_update);

    filter.pid = proc_set->target_pid;
    filter.include_children = proc_set->include_children;
    filter.read_cmd = 0;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(scan_proc);
        return -1;
    }

    /*
     * Clear process list (will be rebuilt from scratch). Only the list
     * nodes are released: the records they pointed at belong to
     * proc_table, which must keep them so that the next cycle can still
     * compare cpu_time against the previous sample. This is the point of
     * the ownership contract documented on struct process_set.
     */
    clear_list(proc_set->proc_list);

    while (get_next_process(&iter, scan_proc) != -1) {
        struct process *proc;
        /*
         * Never track cpulimit itself: with --include-children the target
         * may be an ancestor of this process and would be reported as a
         * member. Suspending it would stop the limiter before it can run
         * the cleanup that resumes the group, and SIGSTOP cannot be caught
         * or blocked, so the whole group would stay suspended.
         */
        if (scan_proc->pid == self_pid) {
            continue;
        }
        /*
         * Never track PID 1 (init): like cpulimit itself it must never
         * receive a kill. This guard holds for every mode (PID, name,
         * command) and every platform, with no exception, so even a target
         * that resolves to or spawns init cannot be suspended.
         */
        if (scan_proc->pid == 1) {
            continue;
        }
        /*
         * The target's PID was recycled: this group was built around the
         * process that started at target_start_time, but here is a different
         * one wearing the same PID, so the scan no longer matches the
         * target. Stop and leave the group empty; the caller then exits or,
         * with -e, searches again by name. Only the target is checked --
         * a recycled descendant is decided by is_child_of() from the live
         * parent chain and rarely matches.
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
            proc = process_dup(scan_proc);
            if (proc == NULL) {
                /*
                 * Out of memory: abandon the cycle. Returning -1 makes
                 * limit_process() resume every stopped member in cleanup.
                 */
                alloc_failed = 1;
                break;
            }
            proc->cpu_usage = -1;
            /*
             * The snapshot carries {0,0}, so stamp the baseline with the
             * moment it was taken; a member found later in the cycle gets a
             * correspondingly later baseline.
             */
            proc->cpu_time_ts = now;
            if (add_to_process_table(proc_set->proc_table, proc) != 0) {
                free(proc);
                alloc_failed = 1;
                break;
            }
            if (add_list_elem(proc_set->proc_list, proc) == NULL) {
                /*
                 * In the table but not yet the list: drop it to free the
                 * data and abort rather than leak the orphan.
                 */
                delete_from_process_table(proc_set->proc_table, proc->pid);
                alloc_failed = 1;
                break;
            }
        } else {
            /*
             * Re-add a surviving process to this cycle's list. The list was
             * cleared at the top, so a survivor is absent and must return.
             * The same PID can appear twice in one snapshot (a /proc race):
             * its first occurrence already added it and refreshed cpu_time,
             * so re-adding would double-count it and a repeat would compute a
             * bogus near-zero sample that drags the EMA down. Only re-add
             * when it is not already in the list; the accounting below stays
             * inside this first-occurrence branch for that reason.
             */
            if (find_process_in_list_by_pid(proc_set->proc_list, proc->pid) ==
                NULL) {
                if (add_list_elem(proc_set->proc_list, proc) == NULL) {
                    /*
                     * The member could not be linked into this cycle's
                     * view. Besides missing this cycle's SIGSTOP/
                     * SIGCONT, a member absent from proc_list would be
                     * treated as exited by
                     * remove_stale_from_process_table() at the end of the
                     * cycle and lose its CPU history. Abort the cycle
                     * like the other allocation failures. Unlike the
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
     * iterator failed to close. proc_list was rebuilt above and is
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

/**
 * @brief Aggregate CPU usage across all processes in the group
 *
 * @param proc_set Pointer to the process_set structure to query
 * @return Sum of cpu_usage for members with a known measurement, or -1.0 if
 *         none are known yet or proc_set is NULL
 *
 * @note Returns -1 rather than 0 to distinguish "no usage" from "unknown"
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
        if (proc == NULL) {
            continue;
        }
        if (proc->cpu_usage < 0) {
            continue;
        }
        if (cpu_usage < 0) {
            cpu_usage = 0;
        }
        cpu_usage += proc->cpu_usage;
    }
    return cpu_usage;
}

/**
 * @brief Check whether the process set currently has no active members
 *
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
 *
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
 *
 * @param sig Signal whose delivery failed
 * @param pid Process the signal could not be delivered to
 * @param err errno captured at the point of failure
 * @param verbose Retained for API compatibility; the caller already decided
 *                whether to report, so this does not change output
 * @param may_remain_stopped Non-zero when a failed SIGCONT left the member
 *                           stopped, so the hint names the PID to resume
 *
 * A failed SIGCONT for a process that is gone (ESRCH) is not reported;
 * reporting happens without -v because the requested limit cannot be enforced.
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
 * @brief Classify one undelivered signal as benign or severe
 *
 * @param sig The signal whose delivery failed
 * @param proc The member the signal was meant for
 * @param gate Out: the per-member flag that gates this failure kind, so the
 *             caller reports once per episode
 * @param may_remain_stopped Out: non-zero when this failure can have stranded
 *                           the member stopped
 * @return SIGNAL_FAILURE_BENIGN or SIGNAL_FAILURE_SEVERE
 *
 * A failed SIGCONT for a member this group never suspended is benign (it was
 * running all along, nothing stranded); every other failure is severe and may
 * carry the recovery hint.
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
int process_set_send_signal(struct process_set *proc_set, int sig,
                            int verbose) {
    struct list_node *node;
    int failed = 0;

    /*
     * Resume recorded PIDs first: those have left the group, so a group
     * whose list is gone still owes them a SIGCONT. Failure there counts
     * like a failed resume of a current member.
     */
    if (sig == SIGCONT) {
        failed = resume_stopped_pids(proc_set);
    }
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        /*
         * Return what the deferred round reported rather than a bare 0:
         * the group list may be gone while recorded suspensions remain.
         */
        return failed;
    }

    node = first_list_node(proc_set->proc_list);
    while (node != NULL) {
        struct list_node *next_node = node->next;
        struct process *proc;
        pid_t pid;
        int kill_result;
        if (node->data == NULL) {
            delete_list_node(proc_set->proc_list, node);
            node = next_node;
            continue;
        }
        proc = (struct process *)node->data;
        pid = proc->pid;
        kill_result = kill(pid, sig);

        if (kill_result != 0) {
            /*
             * Save errno: kill() is non-blocking, cannot return EINTR, and a
             * failure means the process can no longer be controlled (ESRCH:
             * gone; EPERM/EACCES: refused).
             */
            int saved_errno = errno;
            int *gate;
            int may_remain_stopped;
            if (saved_errno == ESRCH) {
                /*
                 * The process is gone: drop its suspension record too.
                 * resume_stopped_pids() would otherwise still try to resume
                 * this PID, which may already have been recycled.
                 */
                forget_stopped_pid(proc_set, pid);
                delete_list_node(proc_set->proc_list, node);
                delete_from_process_table(proc_set->proc_table, pid);
            } else {
                /*
                 * Still alive but refused the signal (EPERM/EACCES, a seccomp
                 * filter, ...). Keep tracking it: dropping it would silently
                 * end the limit and let it run past budget with no output
                 * explaining why; its CPU time still counts, keeping the rest
                 * of the group honest.
                 */
                /*
                 * Report once per episode: a member retried every cycle would
                 * otherwise flood the terminal, and a successful delivery
                 * clears the flag. A failed SIGCONT counts as "may remain
                 * stopped" only for a member this group suspended.
                 */
                int kind;
                kind = classify_signal_failure(sig, proc, &gate,
                                               &may_remain_stopped);
                /*
                 * Report once per episode via the flag the classifier
                 * picked.
                 */
                if (!*gate) {
                    warn_signal_failure(sig, pid, saved_errno, verbose,
                                        may_remain_stopped);
                }
                *gate = 1;
                /* Only a severe failure counts and can strand a member. */
                if (kind == SIGNAL_FAILURE_SEVERE) {
                    failed++;
                    if (sig == SIGCONT && proc->suspended_by_us) {
                        /*
                         * Re-record the suspension this group still owes an
                         * undo for. The round destroyed every record, so this
                         * stopped member has none left: if it leaves the group
                         * (e.g. a descendant re-parented when its ancestor
                         * exits) the table entry keeping it visible goes too,
                         * and nothing would ever resume it.
                         * record_stopped_pid() folds a repeat into the first,
                         * so no duplicate is created, and a failed allocation
                         * resumes it at once.
                         */
                        (void)record_stopped_pid(proc_set, pid,
                                                 proc->start_time);
                    }
                }
            }
        } else if (sig == SIGSTOP) {
            /*
             * Record the suspension so it can be undone, and set the flag
             * only when the record truly exists: otherwise record_stopped_pid()
             * already resumed the member and the flag would claim a suspension
             * that was taken back.
             */
            if (record_stopped_pid(proc_set, pid, proc->start_time) == 0) {
                proc->suspended_by_us = 1;
            }
            /*
             * A successful delivery ends this signal's failure episode, so an
             * early SIGSTOP failure cannot silence every later one.
             */
            proc->stop_warned = 0;
        } else if (sig == SIGCONT) {
            /*
             * SIGCONT delivered: clear the warnable state so a later failure
             * re-reports instead of going unnoticed. The suspension this flag
             * tracks is undone as well.
             *
             * Spelled out rather than left as the catch-all else: only a
             * SIGCONT undoes a suspension, so clearing these for any other
             * signal would leave a still-stopped member unrecorded -- no
             * closing SIGCONT would reach it, no "may remain stopped" hint
             * would name it, and it would stay stopped with nothing said.
             */
            proc->cont_warned = 0;
            proc->resume_warned = 0;
            proc->suspended_by_us = 0;
        }
        node = next_node;
    }
    return failed;
}
