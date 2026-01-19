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

#ifdef __FreeBSD__

#ifndef __PROCESS_ITERATOR_FREEBSD_C
#define __PROCESS_ITERATOR_FREEBSD_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(__STRICT_ANSI__) || !defined(__STDC_VERSION__) ||                  \
    (__STDC_VERSION__ < 199901L)
#define inline __inline
#endif

#include <fcntl.h>
#include <kvm.h>
#include <limits.h>
#include <paths.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>

#include "process_iterator.h"

/**
 * @brief Initialize a process iterator
 * @param it Pointer to the process_iterator structure
 * @param filter Pointer to the process_filter structure
 * @return 0 on success, exits with error on failure
 */
int init_process_iterator(struct process_iterator *it,
                          const struct process_filter *filter) {
    const struct kinfo_proc *procs;
    char *errbuf;

    it->filter = filter;

    if ((errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX)) == NULL) {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }

    /* Open the kvm interface, get a descriptor */
    if ((it->kd = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf)) ==
        NULL) {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
        free(errbuf);
        return -1;
    }
    free(errbuf);

    if (it->filter->pid != 0 && !it->filter->include_children) {
        /* In this case, it->procs is never used */
        it->procs = NULL;
        it->i = -1;
        it->count = 0;
        return 0;
    }

    /* Get the list of processes. */
    if ((procs = kvm_getprocs(it->kd, KERN_PROC_PROC, 0, &it->count)) == NULL) {
        kvm_close(it->kd);
        return -1;
    }
    if ((it->procs = (struct kinfo_proc *)malloc(sizeof(struct kinfo_proc) *
                                                 (size_t)it->count)) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        exit(EXIT_FAILURE);
    }
    memcpy(it->procs, procs, sizeof(struct kinfo_proc) * (size_t)it->count);
    it->i = 0;
    return 0;
}

/**
 * @brief Convert kinfo_proc structure to process structure
 * @param kd Kernel virtual memory descriptor
 * @param kproc Pointer to source kinfo_proc structure
 * @param proc Pointer to destination process structure
 * @param read_cmd Flag indicating whether to read command line
 * @return 0 on success, -1 on failure
 */
static int kproc2proc(kvm_t *kd, struct kinfo_proc *kproc, struct process *proc,
                      int read_cmd) {
    char **args;
    size_t len_max;
    if (kproc == NULL || proc == NULL) {
        return -1;
    }
    memset(proc, 0, sizeof(struct process));
    proc->pid = kproc->ki_pid;
    proc->ppid = kproc->ki_ppid;
    proc->cputime = (double)kproc->ki_runtime / 1000.0;
    if (!read_cmd) {
        return 0;
    }
    len_max = sizeof(proc->command) - 1;
    args = kvm_getargv(kd, kproc, (int)len_max);
    if (args == NULL || args[0] == NULL) {
        return -1;
    }
    strncpy(proc->command, args[0], len_max);
    proc->command[len_max] = '\0';
    return 0;
}

/**
 * @brief Get information for a single process by PID
 * @param kd Kernel virtual memory descriptor
 * @param pid Process ID to query
 * @param process Pointer to process structure to fill
 * @param read_cmd Flag indicating whether to read command line
 * @return 0 on success, -1 on failure
 */
static int get_single_process(kvm_t *kd, pid_t pid, struct process *process,
                              int read_cmd) {
    int count;
    struct kinfo_proc *kproc = kvm_getprocs(kd, KERN_PROC_PID, pid, &count);
    if (count == 0 || kproc == NULL ||
        kproc2proc(kd, kproc, process, read_cmd) != 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief Get the parent process ID of a given PID (internal helper)
 * @param kd Kernel virtual memory descriptor
 * @param pid Process ID to query
 * @return Parent process ID, or -1 on error
 */
static pid_t _getppid_of(kvm_t *kd, pid_t pid) {
    int count;
    struct kinfo_proc *kproc = kvm_getprocs(kd, KERN_PROC_PID, pid, &count);
    return (count == 0 || kproc == NULL) ? (pid_t)(-1) : kproc->ki_ppid;
}

/**
 * @brief Get the parent process ID (PPID) of a given PID
 * @param pid The given PID
 * @return Parent process ID, or -1 on error
 */
pid_t getppid_of(pid_t pid) {
    pid_t ppid;
    kvm_t *kd;
    char *errbuf;
    if ((errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX)) == NULL) {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }
    kd = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf);
    if (kd == NULL) {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
        free(errbuf);
        return (pid_t)(-1);
    }
    free(errbuf);
    ppid = _getppid_of(kd, pid);
    kvm_close(kd);
    return ppid;
}

/**
 * @brief Check if a process is a child of another process (internal helper)
 * @param kd Kernel virtual memory descriptor
 * @param child_pid Potential child process ID
 * @param parent_pid Potential parent process ID
 * @return 1 if child_pid is a child of parent_pid, 0 otherwise
 */
static int _is_child_of(kvm_t *kd, pid_t child_pid, pid_t parent_pid) {
    if (child_pid <= 0 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    /* Traverse parent chain to check ancestry */
    while (child_pid > 1 && child_pid != parent_pid) {
        child_pid = _getppid_of(kd, child_pid);
    }
    return child_pid == parent_pid;
}

/**
 * @brief Check if a process is a child of another process
 * @param child_pid Potential child process ID
 * @param parent_pid Potential parent process ID
 * @return 1 if child_pid is a child of parent_pid, 0 otherwise
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    int ret;
    kvm_t *kd;
    char *errbuf;
    if ((errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX)) == NULL) {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }
    kd = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf);
    if (kd == NULL) {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
        exit(EXIT_FAILURE);
    }
    free(errbuf);
    ret = _is_child_of(kd, child_pid, parent_pid);
    kvm_close(kd);
    return ret;
}

/**
 * @brief Get the next process matching the filter criteria
 * @param it Pointer to the process_iterator structure
 * @param p Pointer to the process structure to store process information
 * @return 0 on success, -1 if no more processes are available
 */
int get_next_process(struct process_iterator *it, struct process *p) {
    if (it == NULL || p == NULL) {
        return -1;
    }

    if (it->i >= it->count) {
        return -1;
    }

    /* Single PID without children */
    if (it->filter->pid != 0 && !it->filter->include_children) {
        if (get_single_process(it->kd, it->filter->pid, p,
                               it->filter->read_cmd) != 0) {
            it->i = it->count = 0;
            return -1;
        }
        it->i = it->count = 1;
        return 0;
    }

    while (it->i < it->count) {
        struct kinfo_proc *kproc = &it->procs[it->i++];
        /* Skip system and zombie processes */
        if ((kproc->ki_flag & P_SYSTEM) || (kproc->ki_stat == SZOMB)) {
            continue;
        }
        /*
         * If filtering by PID + children, try to reject early
         * using kinfo_proc data only.
         */
        if (it->filter->pid != 0 && it->filter->include_children &&
            kproc->ki_pid != it->filter->pid &&
            !_is_child_of(it->kd, kproc->ki_pid, it->filter->pid)) {
            continue;
        }

        /*
         * Now do the expensive conversion only if needed
         */
        if (kproc2proc(it->kd, kproc, p, it->filter->read_cmd) != 0) {
            continue;
        }

        return 0;
    }

    return -1;
}

/**
 * @brief Close the process iterator and free resources
 * @param it Pointer to the process_iterator structure
 * @return 0 on success, -1 on failure
 */
int close_process_iterator(struct process_iterator *it) {
    if (it->procs != NULL) {
        free(it->procs);
        it->procs = NULL;
    }
    if (kvm_close(it->kd) != 0) {
        perror("kvm_close");
        return -1;
    }
    return 0;
}

#endif
#endif
