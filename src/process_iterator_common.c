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

#include "process_iterator.h"

#include <stddef.h>

/**
 * @brief Determine whether a process ID satisfies the iterator filter
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
