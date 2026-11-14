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
 * @brief Compare two process start times for approximate equality.
 *
 * @param a First start time, or UNKNOWN_START_TIME.
 * @param b Second start time, or UNKNOWN_START_TIME.
 * @return 1 when the readings are approximately equal, 0 otherwise.
 *
 * @note Shared by PID-reuse checks so both use one definition of "same
 *       process". Callers must handle UNKNOWN_START_TIME themselves.
 */
int start_time_matches(double a, double b) {
    double diff = a - b;
    double scale = a < 0.0 ? -a : a;
    double other = b < 0.0 ? -b : b;
    double tol;
    if (other > scale) {
        scale = other;
    }
    /* Scale the tolerance so a one-ULP drift still compares equal. */
    tol = DBL_EPSILON * scale;
    if (tol < DBL_EPSILON) {
        tol = DBL_EPSILON;
    }
    return diff >= -tol && diff <= tol;
}

/**
 * @def PROCESS_TABLE_HASHSIZE
 * @brief Number of hash buckets for the process table.
 */
#define PROCESS_TABLE_HASHSIZE 2048

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
    /* An initial scan must not reject the target as recycled. */
    proc_set->target_start_time = UNKNOWN_START_TIME;
    if (update_process_set(proc_set) != 0) {
        fprintf(stderr, "Failed to perform initial process group scan\n");
        close_process_set(proc_set);
        return -1;
    }
    /* Remember the target's start time to detect PID reuse. */
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
 * @brief Release every resource owned by a process set.
 *
 * @param proc_set Set to clean up.
 * @return 0, always.
 *
 * @note Safe to call with NULL or a partially initialized set.
 */
int close_process_set(struct process_set *proc_set) {
    if (proc_set == NULL) {
        return 0;
    }
    if (proc_set->proc_list != NULL) {
        /* clear_list: the records are owned by proc_table. */
        clear_list(proc_set->proc_list);
        free(proc_set->proc_list);
        proc_set->proc_list = NULL;
    }

    if (proc_set->stopped_pids != NULL) {
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
 * @brief Duplicate a process structure.
 *
 * @param proc Source process.
 * @return The copy, or NULL on an allocation failure.
 */
static struct process *process_dup(const struct process *proc) {
    struct process *new_proc;
    new_proc = (struct process *)malloc(sizeof(struct process));
    if (new_proc == NULL) {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        return NULL;
    }
    /* memcpy avoids a 4 KiB stack temporary. */
    memcpy(new_proc, proc, sizeof(*new_proc));
    return new_proc;
}

/* A suspension record: PID, start time and a warning flag. */
struct stopped_pid_record {
    pid_t pid;
    double start_time;
    int warned;
};

/**
 * @brief Undo a suspension that could not be recorded.
 *
 * @param pid PID that received SIGSTOP.
 * @return RECORD_STOPPED_RESUMED or RECORD_STOPPED_STILL_STOPPED.
 *
 * @note PID 1 is dropped without a signal instead.
 */
static int undo_unrecorded_suspension(pid_t pid) {
    /* Never signal PID 1, even to undo a suspension. */
    if (pid == 1) {
        return RECORD_STOPPED_RESUMED;
    }
    if (kill(pid, SIGCONT) != 0 && errno != ESRCH) {
        return RECORD_STOPPED_STILL_STOPPED;
    }
    return RECORD_STOPPED_RESUMED;
}

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
                       double start_time) {
    struct stopped_pid_record *rec;
    struct list_node *node;

    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return undo_unrecorded_suspension(pid);
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
    if (rec != NULL) {
        rec->pid = pid;
        rec->start_time = start_time;
        rec->warned = 0;
        if (add_list_elem(proc_set->stopped_pids, rec) != NULL) {
            return 0;
        }
        free(rec);
    }
    /* Undo an unrecordable suspension at once so the member is not stranded. */
    return undo_unrecorded_suspension(pid);
}

static void warn_signal_failure(int sig, pid_t pid, int err, int verbose,
                                int may_remain_stopped);

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
int resume_stopped_pids(struct process_set *proc_set, int verbose) {
    struct list_node *node, *next_node;
    int failed = 0;
    if (proc_set == NULL || proc_set->stopped_pids == NULL) {
        return 0;
    }
    for (node = first_list_node(proc_set->stopped_pids); node != NULL;
         node = next_node) {
        struct stopped_pid_record *rec =
            (struct stopped_pid_record *)node->data;
        pid_t pid = rec->pid;
        int keep_record = 0;
        next_node = node->next;
        /* Never signal PID 1, even for a deferred resume. */
        if (pid == 1) {
            destroy_list_node(proc_set->stopped_pids, node);
            continue;
        }
        /* Current members are resumed by the regular SIGCONT round. */
        if (find_process_in_list_by_pid(proc_set->proc_list, pid) == NULL) {
            int resumed = 1;
            /* Recheck the start time so a recycled PID is not resumed. */
            if (!start_time_matches(rec->start_time, UNKNOWN_START_TIME)) {
                double current = get_process_start_time(pid);
                /* A changed start time does not rule out a stopped owner. */
                if (!start_time_matches(current, UNKNOWN_START_TIME) &&
                    !start_time_matches(current, rec->start_time) &&
                    !process_is_stopped(pid)) {
                    resumed = 0;
                }
            }
            /* Keep the record on a non-ESRCH failure for a later retry. */
            if (resumed && kill(pid, SIGCONT) != 0) {
                int err = errno;
                /* Warn once for a retained record. */
                warn_signal_failure(SIGCONT, pid, err, verbose && !rec->warned,
                                    1);
                rec->warned = 1;
                if (err != ESRCH) {
                    failed++;
                    keep_record = 1;
                }
            }
        }
        if (!keep_record) {
            destroy_list_node(proc_set->stopped_pids, node);
        }
    }
    return failed;
}

/**
 * @brief Drop a suspension record without resuming the PID.
 *
 * @param proc_set Set owning the record.
 * @param pid PID to forget.
 *
 * @note Used after a failed signal: the PID may have been recycled, so a
 *       later resume could reach an unrelated process.
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
 * @brief Smoothing factor for the CPU-usage EMA.
 */
#define CPU_EMA_ALPHA 0.4

/**
 * @def CPU_MIN_DELTA_MS
 * @brief Minimum interval in milliseconds for a valid CPU sample.
 */
#define CPU_MIN_DELTA_MS 20

/**
 * @brief Update a tracked entry with its latest scan sample.
 *
 * @param proc Stored entry to update.
 * @param scan_proc Fresh sample of the same process.
 * @param now Timestamp of this cycle.
 * @param ncpu Number of CPUs, used to cap the sample.
 *
 * @note The interval comes from proc->cpu_time_ts, so a member found
 *       mid-cycle keeps its own baseline.
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
        int was_suspended;
        /* A drop in CPU time or a changed start time means PID reuse. */
        was_suspended = proc->suspended_by_us;
        memcpy(proc, scan_proc, sizeof(*proc));
        proc->cpu_usage = -1;
        proc->cpu_time_ts = *now;
        /* Preserve suspended_by_us: the snapshot would clear it. */
        proc->suspended_by_us = was_suspended;
        return;
    }
    proc->ppid = scan_proc->ppid;
    /* Fill in a start time that was unreadable at discovery. */
    if (start_time_matches(proc->start_time, UNKNOWN_START_TIME) &&
        !start_time_matches(scan_proc->start_time, UNKNOWN_START_TIME)) {
        proc->start_time = scan_proc->start_time;
    }
    elapsed_ms = timediff_in_ms(now, &proc->cpu_time_ts);
    if (elapsed_ms < 0) {
        /* A backward clock keeps cpu_time but skips usage this cycle. */
        proc->cpu_time = scan_proc->cpu_time;
        proc->cpu_time_ts = *now;
        proc->cpu_usage = -1;
        return;
    }
    if (elapsed_ms < CPU_MIN_DELTA_MS) {
        /* A too-short interval keeps cpu_time for the next update. */
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
 * @brief Refresh the process set and recalculate CPU usage.
 *
 * @param proc_set Set to update.
 * @return 0 on success, -1 on an iterator, clock or allocation error; the
 *         caller must then break its loop so cleanup can resume the group.
 *
 * @note A NULL proc_set returns 0 immediately.
 */
int update_process_set(struct process_set *proc_set) {
    struct process_iterator iter;
    struct process *scan_proc;
    struct process_filter filter;
    struct timespec now;
    double elapsed_ms;
    int ncpu, close_ret, scan_status;
    pid_t self_pid;
    int target_replaced;
    int alloc_failed;
    if (proc_set == NULL || proc_set->proc_list == NULL ||
        proc_set->proc_table == NULL) {
        return 0;
    }
    ncpu = get_ncpu();
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

    /* Only list nodes are freed; proc_table owns the records. */
    clear_list(proc_set->proc_list);

    while ((scan_status = get_next_process(&iter, scan_proc)) == 0) {
        struct process *proc;
        /* Never track cpulimit itself: SIGSTOP would halt the cleanup. */
        if (scan_proc->pid == self_pid) {
            continue;
        }
        /* Never track PID 1 on any platform or in any mode. */
        if (scan_proc->pid == 1) {
            continue;
        }
        /* A recycled target PID empties the group for re-resolution. */
        if (!start_time_matches(proc_set->target_start_time,
                                UNKNOWN_START_TIME) &&
            !start_time_matches(scan_proc->start_time, UNKNOWN_START_TIME) &&
            scan_proc->pid == proc_set->target_pid &&
            !start_time_matches(scan_proc->start_time,
                                proc_set->target_start_time)) {
            /* An empty list alone looks like an exit. */
            target_replaced = 1;
            proc_set->target_replaced = 1;
            break;
        }
        proc = find_in_process_table(proc_set->proc_table, scan_proc->pid);
        if (proc == NULL) {
            proc = process_dup(scan_proc);
            if (proc == NULL) {
                /* Abort the cycle so the caller resumes the group. */
                alloc_failed = 1;
                break;
            }
            proc->cpu_usage = -1;
            /* Stamp the baseline at the moment the snapshot was taken. */
            proc->cpu_time_ts = now;
            if (add_to_process_table(proc_set->proc_table, proc) != 0) {
                free(proc);
                alloc_failed = 1;
                break;
            }
            if (add_list_elem(proc_set->proc_list, proc) == NULL) {
                /* Drop an orphaned record rather than leak it. */
                delete_from_process_table(proc_set->proc_table, proc->pid);
                alloc_failed = 1;
                break;
            }
        } else {
            /* A duplicate /proc entry must not be added twice. */
            if (find_process_in_list_by_pid(proc_set->proc_list, proc->pid) ==
                NULL) {
                if (add_list_elem(proc_set->proc_list, proc) == NULL) {
                    /* Abort the cycle; the record stays owned by proc_table. */
                    alloc_failed = 1;
                    break;
                }
                update_existing_process_entry(proc, scan_proc, &now, ncpu);
            }
        }
    }
    if (target_replaced) {
        /* Drop the partial list so the group reads as empty. */
        clear_list(proc_set->proc_list);
    }
    free(scan_proc);
    close_ret = close_process_iterator(&iter);
    if (close_ret != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }

    /* Purge stale entries only after a complete scan. */
    /* A partial scan keeps records for members it never reached. */
    if (scan_status != PROCESS_ITER_ERROR && !alloc_failed) {
        remove_stale_from_process_table(proc_set->proc_table,
                                        proc_set->proc_list);
    }

    if (close_ret != 0) {
        return -1;
    }
    if (alloc_failed) {
        /* The caller breaks its loop and resumes the group. */
        return -1;
    }
    if (scan_status == PROCESS_ITER_ERROR) {
        /* Fail the cycle rather than act on a partial process table. */
        return -1;
    }

    /* Update the timestamp only after a measurable interval. */
    if (elapsed_ms < 0 || elapsed_ms >= CPU_MIN_DELTA_MS) {
        proc_set->last_update = now;
    }
    return 0;
}

/**
 * @brief Aggregate CPU usage across the group.
 *
 * @param proc_set Set to query.
 * @return Sum of known cpu_usage values, or -1.0 when none is known.
 *
 * @note -1 distinguishes "unknown" from "no usage".
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
 * @brief Check whether the set has no active members.
 *
 * @param proc_set Set to query.
 * @return Non-zero when proc_list is empty or proc_set is NULL.
 */
int process_set_is_empty(const struct process_set *proc_set) {
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return 1;
    }
    return is_empty_list(proc_set->proc_list);
}

/**
 * @brief Return the number of active members.
 *
 * @param proc_set Set to query.
 * @return Number of nodes in proc_list, or 0 when proc_set is NULL.
 */
size_t process_set_member_count(const struct process_set *proc_set) {
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        return 0;
    }
    return proc_set->proc_list->count;
}

/**
 * @brief Report a signal that could not be delivered.
 *
 * @param sig Signal whose delivery failed.
 * @param pid Target PID.
 * @param err errno captured at the failure.
 * @param verbose Non-zero to print.
 * @param may_remain_stopped Non-zero when the member may still be stopped.
 */
static void warn_signal_failure(int sig, pid_t pid, int err, int verbose,
                                int may_remain_stopped) {
    if (!verbose) {
        return;
    }
    if (sig == SIGCONT && err == ESRCH) {
        return;
    }
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
 * @brief classify_signal_failure(): the member kept running.
 */
#define SIGNAL_FAILURE_BENIGN 0

/**
 * @def SIGNAL_FAILURE_SEVERE
 * @brief classify_signal_failure(): the failure may have stranded a member.
 */
#define SIGNAL_FAILURE_SEVERE 1

/**
 * @brief Classify an undelivered signal as benign or severe.
 *
 * @param sig Signal whose delivery failed.
 * @param proc Target member.
 * @param gate Out: per-member flag gating this failure kind.
 * @param may_remain_stopped Out: non-zero when a member may be stopped.
 * @return SIGNAL_FAILURE_BENIGN or SIGNAL_FAILURE_SEVERE.
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
int process_set_send_signal(struct process_set *proc_set, int sig,
                            int verbose) {
    struct list_node *node;
    int failed = 0;

    /* Resume recorded PIDs that have left the group first. */
    if (sig == SIGCONT) {
        failed = resume_stopped_pids(proc_set, verbose);
    }
    if (proc_set == NULL || proc_set->proc_list == NULL) {
        /* The deferred round's failures still count. */
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
        /* Never signal PID 1, even for a set built outside a scan. */
        if (pid == 1) {
            node = next_node;
            continue;
        }
        /* Confirm the PID identity before sending SIGSTOP. */
        if (sig == SIGSTOP) {
            double stop_start = get_process_start_time(pid);
            if (!start_time_matches(proc->start_time, UNKNOWN_START_TIME) &&
                !start_time_matches(stop_start, UNKNOWN_START_TIME) &&
                !start_time_matches(stop_start, proc->start_time)) {
                node = next_node;
                continue;
            }
        }
        kill_result = kill(pid, sig);

        if (kill_result != 0) {
            int saved_errno = errno;
            int *gate;
            int may_remain_stopped;
            if (saved_errno == ESRCH) {
                /* Drop the suspension record of a process that is gone. */
                forget_stopped_pid(proc_set, pid);
                delete_list_node(proc_set->proc_list, node);
                delete_from_process_table(proc_set->proc_table, pid);
            } else {
                /* Keep tracking a member that refused the signal. */
                /* Report once per episode; a success clears the flag. */
                int kind;
                kind = classify_signal_failure(sig, proc, &gate,
                                               &may_remain_stopped);
                if (!*gate) {
                    warn_signal_failure(sig, pid, saved_errno, verbose,
                                        may_remain_stopped);
                }
                *gate = 1;
                /* Only a severe failure counts and can strand a member. */
                if (kind == SIGNAL_FAILURE_SEVERE) {
                    int resumed = 0;
                    if (sig == SIGCONT && proc->suspended_by_us) {
                        /* Re-record the suspension still owed. */
                        resumed = record_stopped_pid(proc_set, pid,
                                                     proc->start_time) ==
                                  RECORD_STOPPED_RESUMED;
                        if (resumed) {
                            proc->suspended_by_us = 0;
                        }
                    }
                    /* An undelivered SIGSTOP cannot strand a running member. */
                    if (!resumed && may_remain_stopped) {
                        failed++;
                    }
                }
            }
        } else if (sig == SIGSTOP) {
            /* Record the suspension only while the member is still stopped. */
            if (record_stopped_pid(proc_set, pid, proc->start_time) !=
                RECORD_STOPPED_RESUMED) {
                proc->suspended_by_us = 1;
            } else {
                proc->suspended_by_us = 0;
            }
            /* A successful delivery ends the failure episode. */
            proc->stop_warned = 0;
        } else if (sig == SIGCONT) {
            /* Only SIGCONT undoes a suspension and clears the warning flags. */
            proc->cont_warned = 0;
            proc->resume_warned = 0;
            proc->suspended_by_us = 0;
        }
        node = next_node;
    }
    return failed;
}
