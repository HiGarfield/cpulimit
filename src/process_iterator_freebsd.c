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

#ifdef __FreeBSD__

#ifndef __PROCESS_ITERATOR_FREEBSD_C
#define __PROCESS_ITERATOR_FREEBSD_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(__STRICT_ANSI__) || !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#define inline __inline
#endif

#include <fcntl.h>
#include <kvm.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include "process_iterator.h"

int init_process_iterator(struct process_iterator *it, struct process_filter *filter)
{
    const struct kinfo_proc *procs;
    char *errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX);
    if (errbuf == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }
    it->i = 0;
    it->procs = NULL;
    /* Open the kvm interface, get a descriptor */
    if ((it->kd = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf)) == NULL)
    {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
        free(errbuf);
        return -1;
    }
    free(errbuf);
    /* Get the list of processes. */
    if ((procs = kvm_getprocs(it->kd, KERN_PROC_PROC, 0, &it->count)) == NULL)
    {
        kvm_close(it->kd);
        return -1;
    }
    it->procs = (struct kinfo_proc *)malloc(sizeof(struct kinfo_proc) * (size_t)it->count);
    if (it->procs == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        exit(EXIT_FAILURE);
    }

    memcpy(it->procs, procs, sizeof(struct kinfo_proc) * (size_t)it->count);
    it->filter = filter;
    return 0;
}

static int kproc2proc(kvm_t *kd, struct kinfo_proc *kproc, struct process *proc)
{
    char **args;
    size_t len_max;
    proc->pid = kproc->ki_pid;
    proc->ppid = kproc->ki_ppid;
    proc->cputime = (double)kproc->ki_runtime / 1000.0;
    len_max = sizeof(proc->command) - 1;
    if ((args = kvm_getargv(kd, kproc, (int)len_max)) == NULL)
        return -1;
    strncpy(proc->command, args[0], len_max);
    proc->command[len_max] = '\0';
    return 0;
}

static int get_single_process(kvm_t *kd, pid_t pid, struct process *process)
{
    int count;
    struct kinfo_proc *kproc = kvm_getprocs(kd, KERN_PROC_PID, pid, &count);
    if (count == 0 || kproc == NULL || kproc2proc(kd, kproc, process) != 0)
        return -1;
    return 0;
}

static pid_t _getppid_of(kvm_t *kd, pid_t pid)
{
    int count;
    struct kinfo_proc *kproc = kvm_getprocs(kd, KERN_PROC_PID, pid, &count);
    return (count == 0 || kproc == NULL) ? (pid_t)(-1) : kproc->ki_ppid;
}

pid_t getppid_of(pid_t pid)
{
    pid_t ppid;
    kvm_t *kd;
    char *errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX);
    if (errbuf == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }
    kd = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf);
    if (kd == NULL)
    {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
        free(errbuf);
        return (pid_t)(-1);
    }
    free(errbuf);
    ppid = _getppid_of(kd, pid);
    kvm_close(kd);
    return ppid;
}

static int _is_child_of(kvm_t *kd, pid_t child_pid, pid_t parent_pid)
{
    if (child_pid <= 0 || parent_pid <= 0 || child_pid == parent_pid)
        return 0;
    while (child_pid > 1 && child_pid != parent_pid)
    {
        child_pid = _getppid_of(kd, child_pid);
    }
    return child_pid == parent_pid;
}

int is_child_of(pid_t child_pid, pid_t parent_pid)
{
    int ret;
    kvm_t *kd;
    char *errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX);
    if (errbuf == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }
    kd = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf);
    if (kd == NULL)
    {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
        free(errbuf);
        return (pid_t)(-1);
    }
    free(errbuf);
    ret = _is_child_of(kd, child_pid, parent_pid);
    kvm_close(kd);
    return ret;
}

int get_next_process(struct process_iterator *it, struct process *p)
{
    if (it->i >= it->count)
    {
        return -1;
    }
    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        if (get_single_process(it->kd, it->filter->pid, p) != 0)
        {
            it->i = it->count = 0;
            return -1;
        }
        it->i = it->count = 1;
        return 0;
    }
    while (it->i < it->count)
    {
        struct kinfo_proc *kproc = it->procs + it->i;
        if (kproc->ki_flag & P_SYSTEM)
        {
            it->i++;
            /* skip system processes */
            continue;
        }
        if (it->filter->pid != 0 && it->filter->include_children)
        {
            it->i++;
            if (kproc2proc(it->kd, kproc, p) != 0)
                continue;
            if (p->pid != it->filter->pid &&
                !_is_child_of(it->kd, p->pid, it->filter->pid))
                continue;
            return 0;
        }
        else if (it->filter->pid == 0)
        {
            it->i++;
            if (kproc2proc(it->kd, kproc, p) != 0)
                continue;
            return 0;
        }
    }
    return -1;
}

int close_process_iterator(struct process_iterator *it)
{
    free(it->procs);
    it->procs = NULL;
    if (kvm_close(it->kd) == -1)
    {
        perror("kvm_close");
        return -1;
    }
    return 0;
}

#endif
#endif
