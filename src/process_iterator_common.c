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

#include "process_iterator.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/**
 * @brief Determine whether a process ID satisfies the iterator filter
 *
 * @param pid Process ID to evaluate
 * @param filter Filter criteria to apply
 * @return 1 if the process matches the filter, 0 otherwise
 *
 * A process matches when:
 * - no PID filter is set (filter->pid == 0), or
 * - its PID equals the filter PID, or
 * - include_children is enabled and it is a descendant of the filter PID.
 *
 * Returns 0 when filter is NULL.
 */
int process_matches_filter(pid_t pid, const struct process_filter *filter) {
    if (filter == NULL) {
        return 0;
    }
    if (filter->pid == 0) {
        return 1;
    }
    if (pid == filter->pid) {
        return 1;
    }
    if (filter->include_children) {
        return is_child_of(pid, filter->pid);
    }
    return 0;
}

/**
 * @brief Get the start time of a single process, if available
 *
 * @param pid Process ID to query
 * @return The process start time in seconds, or UNKNOWN_START_TIME when the
 *         process does not exist or the platform could not provide a value.
 *
 * A thin wrapper over the process iterator that returns just the start_time
 * field for one PID. Used to confirm a PID still belongs to the same process
 * before acting on it (e.g. sending a deferred signal to a PID that may have
 * been recycled). Callers must treat UNKNOWN_START_TIME as "cannot compare"
 * and fall back to acting on the PID.
 *
 * @note A failed iterator close changes nothing about the result: the reading
 *       was already taken, and downgrading a known start time to "cannot
 *       compare" would be worse than the close failure itself. It is still
 *       reported, because an iterator that does not close cleanly is worth
 *       knowing about before it corrupts a later reading.
 */
double get_process_start_time(pid_t pid) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    double result = UNKNOWN_START_TIME;

    if (pid <= 0) {
        return UNKNOWN_START_TIME;
    }
    proc = (struct process *)malloc(sizeof(*proc));
    if (proc == NULL) {
        return UNKNOWN_START_TIME;
    }
    memset(&filter, 0, sizeof(filter));
    filter.pid = pid;
    if (init_process_iterator(&iter, &filter) != 0) {
        free(proc);
        return UNKNOWN_START_TIME;
    }
    while (get_next_process(&iter, proc) == 0) {
        if (proc->pid == pid) {
            result = proc->start_time;
            break;
        }
    }
    /*
     * The reading is already taken, and every backend has released what it
     * allocated by the time it reports a failed close, so the value stands
     * either way. Downgrading to UNKNOWN_START_TIME on a failed close would
     * tell both callers "cannot compare", and each answers that by signalling
     * the PID on guesswork -- the first by resuming a recycled one, the
     * second by suspending whatever inherited it. The failed close is still
     * reported: it says nothing about this reading, but it does say the
     * platform's iterator is not closing cleanly, and that is worth knowing
     * before it corrupts a later one.
     */
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }
    free(proc);
    return result;
}

/**
 * @brief Determine if one process is a descendant of another
 *
 * @param get_parent How to read one process's parent, platform specific
 * @param context Handle handed back to get_parent on every step
 * @param child_pid Process ID to check for descendant relationship
 * @param parent_pid Process ID of the potential ancestor
 * @return 1 if child_pid is a descendant of parent_pid, 0 otherwise
 *
 * The walk every platform shares; see is_child_of() in the header for the
 * cases this answers 0 in.
 */
int is_child_of_via(process_get_parent_fn get_parent, void *context,
                    pid_t child_pid, pid_t parent_pid) {
    unsigned depth = 0;
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    /* Walk up the parent chain looking for parent_pid */
    while (child_pid > 1 && child_pid != parent_pid) {
        pid_t next_ppid;
        /*
         * Depth limit guards against PID reuse cycles of length >= 2.
         * Example: A has ppid=B and B has ppid=A. The self-parenting check
         * (next_ppid == child_pid) only catches length-1 cycles; without
         * this counter the loop would never terminate on length-2+ cycles.
         */
        if (depth++ >= IS_CHILD_MAX_DEPTH) {
            return 0;
        }
        next_ppid = get_parent(context, child_pid);
        /*
         * A transient lookup failure (e.g. the process exited or became a
         * zombie between the previous step and this one) must not be taken as
         * proof of non-membership: retry once before giving up, so a single
         * -1 does not break the ancestor chain.
         */
        if (next_ppid < 0) {
            next_ppid = get_parent(context, child_pid);
        }
        /*
         * Guard against invalid parent links or self-parenting processes,
         * either of which would cause an infinite loop.
         */
        if (next_ppid <= 0 || next_ppid == child_pid) {
            return 0;
        }
        child_pid = next_ppid;
    }
    return child_pid == parent_pid;
}
