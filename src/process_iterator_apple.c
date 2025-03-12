/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 *
 * Author: Simon Sigurdhsson
 *
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>

int init_process_iterator(struct process_iterator *it, struct process_filter *filter)
{
    size_t len;
    struct kinfo_proc *procs;
    int i;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};

    it->i = 0;
    it->filter = filter;

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

static int pti2proc(struct proc_taskallinfo *ti, struct process *process)
{
    process->pid = (pid_t)ti->pbsd.pbi_pid;
    process->ppid = (pid_t)ti->pbsd.pbi_ppid;
    process->cputime = ti->ptinfo.pti_total_user / 1e6 + ti->ptinfo.pti_total_system / 1e6;
    if (proc_pidpath(process->pid, process->command, sizeof(process->command)) <= 0)
        return -1;
    return 0;
}

static int get_process_pti(pid_t pid, struct proc_taskallinfo *ti)
{
    int bytes = proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, ti, sizeof(*ti));
    if (bytes <= 0)
    {
        if (errno != EPERM && errno != ESRCH)
        {
            perror("proc_pidinfo");
        }
        return -1;
    }
    else if (bytes < (int)sizeof(*ti))
    {
        fprintf(stderr, "proc_pidinfo: too few bytes; expected %lu, got %d\n",
                (unsigned long)sizeof(*ti), bytes);
        return -1;
    }
    return 0;
}

pid_t getppid_of(pid_t pid)
{
    struct proc_taskallinfo ti;
    if (get_process_pti(pid, &ti) == 0)
    {
        return (pid_t)ti.pbsd.pbi_ppid;
    }
    return (pid_t)(-1);
}

int is_child_of(pid_t child_pid, pid_t parent_pid)
{
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid)
        return 0;
    if (parent_pid == 1)
        return 1;
    while (child_pid > 1 && child_pid != parent_pid)
    {
        child_pid = getppid_of(child_pid);
    }
    return child_pid == parent_pid;
}

static int read_process_info(pid_t pid, struct process *p)
{
    struct proc_taskallinfo ti;
    if (get_process_pti(pid, &ti) != 0)
    {
        return -1;
    }
    if (ti.pbsd.pbi_flags & PROC_FLAG_SYSTEM)
    {
        return -1;
    }
    if (pti2proc(&ti, p) != 0)
    {
        return -1;
    }
    return 0;
}

int get_next_process(struct process_iterator *it, struct process *p)
{
    if (it->i >= it->count)
        return -1;
    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        if (read_process_info(it->filter->pid, p) == 0)
        {
            it->i = it->count = 1;
            return 0;
        }
        it->i = it->count = 0;
        return -1;
    }
    while (it->i < it->count)
    {
        if (read_process_info(it->pidlist[it->i], p) != 0)
        {
            it->i++;
            continue;
        }
        if (it->filter->pid != 0 && it->filter->include_children)
        {
            it->i++;
            if (p->pid != it->filter->pid && !is_child_of(p->pid, it->filter->pid))
                continue;
            return 0;
        }
        else if (it->filter->pid == 0)
        {
            it->i++;
            return 0;
        }
    }
    return -1;
}

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
