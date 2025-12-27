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

#ifndef __PROCESS_GROUP_H
#define __PROCESS_GROUP_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <time.h>

/**
 * @struct process_group
 * @brief Structure representing a group of processes for tracking
 */
struct process_group
{
    /** Pointer to the process table for storing process information */
    struct process_table *proctable;
    /** Pointer to the list of processes in this group */
    struct list *proclist;
    /** PID of the target process to monitor */
    pid_t target_pid;
    /** Flag indicating whether to include child processes (1 for yes, 0 for no) */
    int include_children;
    /** Timestamp of the last update for this process group */
    struct timespec last_update;
};

/**
 * @brief Find a process by its process ID
 * @param pid Process ID to search for
 * @return The PID if process exists and can be controlled, -PID if exists
 *         but no permission, 0 if process does not exist
 */
pid_t find_process_by_pid(pid_t pid);

/**
 * @brief Find a process by its executable name
 * @param process_name Name or path of the executable to search for
 * @return Process ID if found, 0 if not found, or -PID for permission error
 * @note If process_name starts with '/', full path comparison is used;
 *       otherwise, only the basename is compared.
 */
pid_t find_process_by_name(const char *process_name);

/**
 * @brief Initialize a process group structure
 * @param pgroup Pointer to the process group structure to initialize
 * @param target_pid PID of the target process
 * @param include_children Flag to include child processes in the group
 * @return 0 on success
 */
int init_process_group(struct process_group *pgroup, pid_t target_pid, int include_children);

/**
 * @brief Clean up and close a process group
 * @param pgroup Pointer to the process group structure to close
 * @return 0 on success
 */
int close_process_group(struct process_group *pgroup);

/**
 * @brief Update the process group with current process information
 * @param pgroup Pointer to the process group structure to update
 * @note This function refreshes the process list, calculates CPU usage,
 *       and removes terminated processes from the group.
 */
void update_process_group(struct process_group *pgroup);

/**
 * @brief Calculate the total CPU usage of the process group
 * @param pgroup Pointer to the process group structure
 * @return Total CPU usage of all processes in the group, or -1 if unknown
 */
double get_process_group_cpu_usage(const struct process_group *pgroup);

/**
 * @brief Remove a process from the process group by its PID
 * @param pgroup Pointer to the process group from which to remove the process
 * @param pid The PID of the process to remove
 * @return 0 if removal is successful, or 1 if process is not found
 */
int remove_process(struct process_group *pgroup, pid_t pid);

#endif
