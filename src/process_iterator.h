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

#ifndef __PROCESS_ITERATOR_H
#define __PROCESS_ITERATOR_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <sys/types.h>
#if defined(__linux__)
#include <dirent.h>
#elif defined(__FreeBSD__)
#include <kvm.h>
#include <sys/param.h>
#elif defined(__APPLE__)
#include <libproc.h>
#endif

/* CMD_BUFF_SIZE */
#if defined(__linux__)
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define CMD_BUFF_SIZE PATH_MAX
#elif defined(__FreeBSD__)
#define CMD_BUFF_SIZE MAXPATHLEN
#elif defined(__APPLE__)
#define CMD_BUFF_SIZE PROC_PIDPATHINFO_MAXSIZE
#endif /* CMD_BUFF_SIZE */

/**
 * @struct process
 * @brief Structure representing a process descriptor
 */
struct process
{
    /** Process ID of the process */
    pid_t pid;
    /** Parent Process ID of the process */
    pid_t ppid;
    /** CPU time used by the process (in milliseconds) */
    double cputime;
    /** Actual CPU usage estimation (value in range 0-NCPU) */
    double cpu_usage;
    /** Absolute path of the executable file */
    char command[CMD_BUFF_SIZE];
};

/**
 * @struct process_filter
 * @brief Structure representing a filter for processes
 */
struct process_filter
{
    /** Process ID to filter */
    pid_t pid;
    /** Flag indicating whether to include child processes (1 for yes, 0 for no) */
    int include_children;
    /** Flag indicating whether to read command of process */
    int read_cmd;
};

/**
 * @struct process_iterator
 * @brief Structure representing an iterator for processes
 * @note This structure provides a way to iterate over processes
 *       in different operating systems with their specific implementations
 */
struct process_iterator
{
#if defined(__linux__)
    /** Directory stream for accessing the /proc filesystem on Linux */
    DIR *dip;
    /** Indicator for the end of processes */
    int end_of_processes;
#elif defined(__FreeBSD__)
    /** Kernel virtual memory descriptor for accessing process information on FreeBSD */
    kvm_t *kd;
    /** Array of process information structures */
    struct kinfo_proc *procs;
    /** Total number of processes retrieved */
    int count;
    /** Current index in the process array */
    int i;
#elif defined(__APPLE__)
    /** Current index in the process list */
    int i;
    /** Total number of processes retrieved */
    int count;
    /** List of process IDs */
    pid_t *pidlist;
#endif
    /** Pointer to a process filter to apply during iteration */
    const struct process_filter *filter;
};

/**
 * @brief Initialize a process iterator
 * @param it Pointer to the process_iterator structure
 * @param filter Pointer to the process_filter structure
 * @return 0 on success, -1 on failure
 */
int init_process_iterator(struct process_iterator *it,
                          const struct process_filter *filter);

/**
 * @brief Get the next process matching the filter criteria
 * @param it Pointer to the process_iterator structure
 * @param p Pointer to the process structure to store process information
 * @return 0 on success, -1 if no more processes are available
 */
int get_next_process(struct process_iterator *it, struct process *p);

/**
 * @brief Close the process iterator and free resources
 * @param it Pointer to the process_iterator structure
 * @return 0 on success, -1 on failure
 */
int close_process_iterator(struct process_iterator *it);

/**
 * @brief Check if a process is a child of another process
 * @param child_pid Potential child process ID
 * @param parent_pid Potential parent process ID
 * @return 1 if child_pid is a child of parent_pid, 0 otherwise
 */
int is_child_of(pid_t child_pid, pid_t parent_pid);

/**
 * @brief Get the parent process ID (PPID) of a given PID
 * @param pid The given PID
 * @return Parent process ID, or -1 on error
 */
pid_t getppid_of(pid_t pid);

#endif
