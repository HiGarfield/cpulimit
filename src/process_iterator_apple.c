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
 *
 * Author: Simon Sigurdhsson
 */

#ifdef __APPLE__

#ifndef __PROCESS_ITERATOR_APPLE_C
#define __PROCESS_ITERATOR_APPLE_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_iterator.h"
#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>

/**
 * @brief Initialize a process iterator
 * @param it Pointer to the process_iterator structure
 * @param filter Pointer to the process_filter structure
 * @return 0 on success, exits with error on failure
 */
int init_process_iterator(struct process_iterator *it, struct process_filter *filter)
{
    size_t len;
    struct kinfo_proc *procs;
    int i;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};

    it->i = 0;
    it->filter = filter;

    /* Special case: single PID without children */
    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        /* In this case, it->pidlist is never used */
        it->pidlist = NULL;
        it->i = -1;
        it->count = 0;
        return 0;
    }

    /* Get the size of all process information */
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0)
    {
        perror("Failed to get process information buffer size");
        exit(EXIT_FAILURE); /* Exit on error */
    }

    /*
     * Double the size of the process information buffer
     * to prevent buffer overflow caused by an increase
     * in the number of processes.
     */
    len <<= 1;

    /* Allocate memory to store process information */
    if ((procs = (struct kinfo_proc *)malloc(len)) == NULL)
    {
        fprintf(stderr, "Memory allocation failed for process information buffer\n");
        exit(EXIT_FAILURE); /* Exit on error */
    }

    /* Get process information */
    if (sysctl(mib, 4, procs, &len, NULL, 0) != 0)
    {
        free(procs);
        perror("Failed to get process information");
        exit(EXIT_FAILURE); /* Exit on error */
    }

    /* Calculate the number of processes */
    it->count = (int)(len / sizeof(struct kinfo_proc));
    if ((it->pidlist = (pid_t *)malloc((size_t)it->count * sizeof(pid_t))) == NULL)
    {
        free(procs);
        fprintf(stderr, "Memory allocation failed for PID list\n");
        exit(EXIT_FAILURE); /* Exit on error */
    }

    /* Fill the PID array */
    for (i = 0; i < it->count; i++)
    {
        it->pidlist[i] = procs[i].kp_proc.p_pid;
    }

    free(procs);
    return 0; /* Success */
}

/**
 * @brief Convert platform-specific time units to milliseconds
 * @param platform_time Time in platform-specific units
 * @return Time in milliseconds
 */
static double platform_time_to_ms(double platform_time)
{
    static double factor = -1;
    if (factor < 0)
    {
        mach_timebase_info_data_t timebase_info;
        mach_timebase_info(&timebase_info);
        factor = (double)timebase_info.numer / (double)timebase_info.denom;
    }
    return platform_time * factor / 1e6;
}

/**
 * @brief Convert proc_taskallinfo structure to process structure
 * @param ti Pointer to source proc_taskallinfo structure
 * @param process Pointer to destination process structure
 * @param read_cmd Flag indicating whether to read command path
 * @return 0 on success, -1 on failure
 */
static int pti2proc(struct proc_taskallinfo *ti, struct process *process,
                    int read_cmd)
{
    process->pid = (pid_t)ti->pbsd.pbi_pid;
    process->ppid = (pid_t)ti->pbsd.pbi_ppid;
    process->cputime = platform_time_to_ms(ti->ptinfo.pti_total_user) +
                       platform_time_to_ms(ti->ptinfo.pti_total_system);
    if (!read_cmd)
    {
        return 0;
    }
    if (proc_pidpath(process->pid, process->command, sizeof(process->command)) <= 0)
    {
        return -1;
    }
    return 0;
}

/**
 * @brief Get process task information for a given PID
 * @param pid Process ID to query
 * @param ti Pointer to structure to store task information
 * @return 0 on success, -1 on failure
 */
static int get_process_pti(pid_t pid, struct proc_taskallinfo *ti)
{
    if (proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, ti, sizeof(*ti)) != (int)sizeof(*ti))
    {
        if (errno != EPERM && errno != ESRCH)
        {
            perror("proc_pidinfo");
        }
        return -1;
    }
    if (ti->pbsd.pbi_status == SZOMB)
    {
        /* Skip zombie processes */
        return -1;
    }
    if (ti->pbsd.pbi_flags & PROC_FLAG_SYSTEM)
    {
        /* Skip system processes */
        return -1;
    }
    return 0;
}

/**
 * @brief Get the parent process ID (PPID) of a given PID
 * @param pid The given PID
 * @return Parent process ID, or -1 on error
 */
pid_t getppid_of(pid_t pid)
{
    struct proc_taskallinfo ti;
    if (get_process_pti(pid, &ti) == 0)
    {
        return (pid_t)ti.pbsd.pbi_ppid;
    }
    return (pid_t)(-1);
}

/**
 * @brief Check if a process is a child of another process
 * @param child_pid Potential child process ID
 * @param parent_pid Potential parent process ID
 * @return 1 if child_pid is a child of parent_pid, 0 otherwise
 */
int is_child_of(pid_t child_pid, pid_t parent_pid)
{
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid)
    {
        return 0;
    }
    if (parent_pid == 1)
    {
        return 1;
    }
    /* Traverse parent chain to check ancestry */
    while (child_pid > 1 && child_pid != parent_pid)
    {
        child_pid = getppid_of(child_pid);
    }
    return child_pid == parent_pid;
}

/**
 * @brief Read process information for a given PID
 * @param pid Process ID to query
 * @param p Pointer to process structure to fill
 * @param read_cmd Flag indicating whether to read command path
 * @return 0 on success, -1 on failure
 */
static int read_process_info(pid_t pid, struct process *p, int read_cmd)
{
    struct proc_taskallinfo ti;
    memset(p, 0, sizeof(struct process));
    if (get_process_pti(pid, &ti) != 0)
    {
        return -1;
    }
    if (pti2proc(&ti, p, read_cmd) != 0)
    {
        return -1;
    }
    return 0;
}

/**
 * @brief Get the next process matching the filter criteria
 * @param it Pointer to the process_iterator structure
 * @param p Pointer to the process structure to store process information
 * @return 0 on success, -1 if no more processes are available
 */
int get_next_process(struct process_iterator *it, struct process *p)
{
    if (it->i >= it->count)
    {
        return -1;
    }
    /* Special case: single PID without children */
    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        if (read_process_info(it->filter->pid, p, it->filter->read_cmd) == 0)
        {
            it->i = it->count = 1;
            return 0;
        }
        it->i = it->count = 0;
        return -1;
    }
    /* Iterate through PID list and apply filters */
    while (it->i < it->count)
    {
        if (read_process_info(it->pidlist[it->i], p, it->filter->read_cmd) == 0)
        {
            if (it->filter->pid == 0 ||
                p->pid == it->filter->pid ||
                (it->filter->include_children && is_child_of(p->pid, it->filter->pid)))
            {
                it->i++;
                return 0;
            }
        }
        it->i++;
    }
    return -1;
}

/**
 * @brief Close the process iterator and free resources
 * @param it Pointer to the process_iterator structure
 * @return 0 on success
 */
int close_process_iterator(struct process_iterator *it)
{
    if (it->pidlist != NULL)
    {
        free(it->pidlist);
        it->pidlist = NULL;
    }
    it->filter = NULL;
    it->count = 0;
    it->i = 0;
    return 0;
}

#endif
#endif
