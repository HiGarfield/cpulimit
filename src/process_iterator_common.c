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
 * @brief Determine whether a PID satisfies the iterator filter.
 *
 * @param pid PID to evaluate.
 * @param filter Filter criteria.
 * @return 1 when the PID matches, 0 otherwise or when filter is NULL.
 *
 * A match is the filter PID itself, or a descendant when include_children
 * is set; a zero filter PID matches every process.
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
 * @brief Read the start time of one process.
 *
 * @param pid PID to query.
 * @return Start time in seconds, or UNKNOWN_START_TIME when unavailable.
 *
 * @note Used to confirm a PID still names the same process. A failed
 *       iterator close is reported but does not change the reading.
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
    /* A failed close does not change the reading already taken. */
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }
    free(proc);
    return result;
}

/**
 * @brief Walk a parent chain using a platform-specific lookup.
 *
 * @param get_parent Reads one process's parent.
 * @param context Handle passed back to get_parent.
 * @param child_pid PID to test.
 * @param parent_pid PID of the potential ancestor.
 * @return 1 when parent_pid is in the ancestry chain, 0 otherwise.
 *
 * @note Shared by every backend; see is_child_of().
 */
int is_child_of_via(process_get_parent_fn get_parent,
                    /* cppcheck-suppress funcArgNamesDifferentUnnamed */
                    void *context, pid_t child_pid, pid_t parent_pid) {
    unsigned depth = 0;
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    while (child_pid > 1 && child_pid != parent_pid) {
        pid_t next_ppid;
        /* Bound the walk against PID-reuse cycles. */
        if (depth++ >= IS_CHILD_MAX_DEPTH) {
            return 0;
        }
        next_ppid = get_parent(context, child_pid);
        /* Retry once: a transient lookup failure is not a broken chain. */
        if (next_ppid < 0) {
            next_ppid = get_parent(context, child_pid);
        }
        /* A non-positive or self parent ends the walk. */
        if (next_ppid <= 0 || next_ppid == child_pid) {
            return 0;
        }
        child_pid = next_ppid;
    }
    return child_pid == parent_pid;
}
