/**
 *
 * cpulimit - a CPU limiter for Linux
 *
 * Copyright (C) 2005-2012, by:  Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef __PROCESS_GROUP_H
#define __PROCESS_GROUP_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <time.h>
#include <sys/types.h>
#include "list.h"
#include "process_iterator.h"
#include "process_table.h"

/**
 * Structure representing a group of processes for tracking.
 */
struct process_group
{
    /* Pointer to the process table for storing process information */
    struct process_table *proctable;

    /* Pointer to the list of processes in this group */
    struct list *proclist;

    /* PID of the target process to monitor */
    pid_t target_pid;

    /* Flag indicating whether to include child processes (1 for yes, 0 for no) */
    int include_children;

    /* Timestamp of the last update for this process group */
    struct timespec last_update;
};

/**
 * Initialize a process group for tracking processes.
 *
 * @param pgroup Pointer to the process group structure to initialize.
 * @param target_pid PID of the target process to track.
 * @param include_children Flag indicating whether to include child processes.
 * @return 0 on success, exits with -1 on memory allocation failure.
 */
int init_process_group(struct process_group *pgroup, pid_t target_pid, int include_children);

/**
 * Update the process group with the latest process information.
 *
 * @param pgroup Pointer to the process group to update.
 */
void update_process_group(struct process_group *pgroup);

/**
 * Close a process group and free associated resources.
 *
 * @param pgroup Pointer to the process group to close.
 * @return 0 on success.
 */
int close_process_group(struct process_group *pgroup);

/**
 * Look for a process by its PID.
 *
 * @param pid The PID of the desired process.
 * @return PID of the found process if successful,
 *         Negative PID if the process does not exist or if the signal fails.
 */
pid_t find_process_by_pid(pid_t pid);

/**
 * Look for a process with a given name.
 *
 * @param process_name The name of the desired process. It can be an absolute path
 *                     to the executable file or just the file name.
 * @return PID of the found process if it is found,
 *         0 if the process is not found,
 *         Negative PID if the process is found but cannot be controlled.
 */
pid_t find_process_by_name(char *process_name);

/**
 * Remove a process from the process group by its PID.
 *
 * @param pgroup Pointer to the process group from which to remove the process.
 * @param pid The PID of the process to remove.
 * @return 0 if removal is successful, or 1 if process is not found.
 */
int remove_process(struct process_group *pgroup, pid_t pid);

#endif
