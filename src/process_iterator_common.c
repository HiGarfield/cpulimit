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
 * @note A failed iterator close is reported but does not change the result:
 *       the reading was already taken, and reporting it is preferable to
 *       downgrading a known start time to "cannot compare".
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
    if (close_process_iterator(&iter) != 0) {
        /*
         * The reading is already taken, and every backend has released what
         * it allocated by the time it reports a failed close, so the value
         * still stands. Reporting it is the whole remedy: downgrading to
         * UNKNOWN_START_TIME here would tell both callers "cannot compare",
         * and each answers that by signalling the PID on guesswork -- the
         * first by resuming a recycled one, the second by suspending whatever
         * inherited it. A close failure says nothing about the reading.
         */
        fprintf(stderr, "Failed to close process iterator\n");
    }
    free(proc);
    return result;
}
