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

#ifndef __PROCESS_ITERATOR_H
#define __PROCESS_ITERATOR_H

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#if defined(__linux__)
#include <linux/limits.h>
#endif
#include <dirent.h>

#ifdef __FreeBSD__
#include <kvm.h>
#endif

/**
 * Structure representing a process descriptor.
 */
struct process
{
    /* Process ID of the process */
    pid_t pid;

    /* Parent Process ID of the process */
    pid_t ppid;

    /* CPU time used by the process (in milliseconds) */
    double cputime;

    /* Actual CPU usage estimation (value in range 0-1) */
    double cpu_usage;

    /* Absolute path of the executable file */
    char command[PATH_MAX];
};

/**
 * Structure representing a filter for processes.
 */
struct process_filter
{
    /* Process ID to filter */
    pid_t pid;

    /* Flag indicating whether to include child processes (1 for yes, 0 for no) */
    int include_children;
};

/**
 * Structure representing an iterator for processes.
 * This structure provides a way to iterate over processes
 * in different operating systems with their specific implementations.
 */
struct process_iterator
{
#if defined(__linux__)
    /* Directory stream for accessing the /proc filesystem on Linux */
    DIR *dip;
#elif defined(__FreeBSD__)
    /* Kernel virtual memory descriptor for accessing process information on FreeBSD */
    kvm_t *kd;

    /* Array of process information structures */
    struct kinfo_proc *procs;

    /* Total number of processes retrieved */
    int count;

    /* Current index in the process array */
    int i;
#elif defined(__APPLE__)
    /* Current index in the process list */
    int i;

    /* Total number of processes retrieved */
    int count;

    /* List of process IDs */
    pid_t *pidlist;
#endif

    /* Pointer to a process filter to apply during iteration */
    struct process_filter *filter;
};

/**
 * Initializes a process iterator.
 *
 * @param it Pointer to the process_iterator structure.
 * @param filter Pointer to the process_filter structure.
 * @return 0 on success, -1 on failure.
 */
int init_process_iterator(struct process_iterator *it, struct process_filter *filter);

/**
 * Retrieves the next process information in the process iterator.
 *
 * @param it Pointer to the process_iterator structure.
 * @param p Pointer to the process structure to store process information.
 * @return 0 on success, -1 if no more processes are available.
 */
int get_next_process(struct process_iterator *it, struct process *p);

/**
 * Closes the process iterator.
 *
 * @param it Pointer to the process_iterator structure.
 * @return 0 on success, 1 on failure.
 */
int close_process_iterator(struct process_iterator *it);

/**
 * Determines if a process is a child of another process.
 *
 * @param child_pid The child process ID.
 * @param parent_pid The parent process ID.
 * @return 1 if the child process is a child of the parent process, 0 otherwise.
 */
int is_child_of(pid_t child_pid, pid_t parent_pid);

/**
 * Retrieves the parent process ID (PPID) of a given PID.
 *
 * @param pid The given PID.
 * @return The parent process ID.
 */
pid_t getppid_of(pid_t pid);

#endif
