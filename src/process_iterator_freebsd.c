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

#ifndef CPULIMIT_PROCESS_ITERATOR_FREEBSD_C
#define CPULIMIT_PROCESS_ITERATOR_FREEBSD_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(__STRICT_ANSI__) || !defined(__STDC_VERSION__) ||                  \
    (__STDC_VERSION__ < 199901L)
#define inline
#endif

#include "process_iterator.h"

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

/**
 * @brief Open the kvm(3) interface read-only.
 *
 * @return An open kvm descriptor, or NULL on failure.
 */
static kvm_t *open_kvm(void) {
    kvm_t *kvm_descriptor;
    char *errbuf;
    errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX);
    if (errbuf == NULL) {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        return NULL;
    }
    /* kvm_openfiles() may leave the error buffer untouched. */
    errbuf[0] = '\0';
    kvm_descriptor = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf);
    if (kvm_descriptor == NULL) {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
    }
    free(errbuf);
    return kvm_descriptor;
}

/**
 * @brief Initialize a process iterator with the given filter.
 *
 * @param iter Iterator to initialize.
 * @param filter Filter criteria, which must stay valid during iteration.
 * @return 0 on success, -1 on a NULL argument, an allocation failure or a
 *         backend-specific initialization error; never calls exit().
 *
 * @note The filter pointer is stored until close_process_iterator().
 */
int init_process_iterator(struct process_iterator *iter,
                          const struct process_filter *filter) {
    const struct kinfo_proc *proc_snapshot;
    size_t size_max = 0, snapshot_size;

    if (iter == NULL || filter == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->filter = filter;

    iter->kvm_descriptor = open_kvm();
    if (iter->kvm_descriptor == NULL) {
        return -1;
    }

    /* A single PID without children needs no snapshot. */
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        iter->kinfo_procs = NULL;
        iter->current_index = 0;
        iter->proc_count = 1;
        return 0;
    }

    proc_snapshot = kvm_getprocs(iter->kvm_descriptor, KERN_PROC_PROC, 0,
                                 &iter->proc_count);
    if (proc_snapshot == NULL) {
        fprintf(stderr, "kvm_getprocs: %s\n", kvm_geterr(iter->kvm_descriptor));
        close_process_iterator(iter);
        return -1;
    }
    /* A non-positive count would wrap when cast to size_t. */
    if (iter->proc_count <= 0) {
        fprintf(stderr, "kvm_getprocs: unexpected proc_count %d\n",
                iter->proc_count);
        close_process_iterator(iter);
        return -1;
    }
    /* Reject an overflowing snapshot allocation before malloc(). */
    size_max--;
    if ((size_t)iter->proc_count > size_max / sizeof(struct kinfo_proc)) {
        fprintf(stderr, "kvm_getprocs: process snapshot is too large: %d\n",
                iter->proc_count);
        close_process_iterator(iter);
        return -1;
    }
    snapshot_size = sizeof(struct kinfo_proc) * (size_t)iter->proc_count;
    iter->kinfo_procs = (struct kinfo_proc *)malloc(snapshot_size);
    if (iter->kinfo_procs == NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        close_process_iterator(iter);
        return -1;
    }
    memcpy(iter->kinfo_procs, proc_snapshot, snapshot_size);
    iter->current_index = 0;
    return 0;
}

/**
 * @brief Convert kinfo_proc to the portable process structure.
 *
 * @param kvm_descriptor Descriptor used for kvm_getargv().
 * @param kproc Source structure.
 * @param proc Destination structure.
 * @param read_cmd Non-zero to read argv[0].
 * @return 0 on success, -1 on failure.
 */
static int kinfo_proc_to_proc(kvm_t *kvm_descriptor, struct kinfo_proc *kproc,
                              struct process *proc, int read_cmd) {
    char **args;
    int len_max;
    if (kproc == NULL || proc == NULL) {
        return -1;
    }
    memset(proc, 0, sizeof(struct process));
    proc->pid = kproc->ki_pid;
    proc->ppid = kproc->ki_ppid;
    proc->cpu_time = (double)kproc->ki_runtime / 1000.0;
    proc->start_time =
        (double)kproc->ki_start.tv_sec + (double)kproc->ki_start.tv_usec / 1e6;
    if (!read_cmd) {
        return 0;
    }
    len_max = (int)(sizeof(proc->command) - 1);
    args = kvm_getargv(kvm_descriptor, kproc, len_max);
    if (args == NULL || args[0] == NULL) {
        return -1;
    }
    strncpy(proc->command, args[0], (size_t)len_max);
    proc->command[len_max] = '\0';
    if (proc->command[0] == '\0') {
        return -1;
    }
    return 0;
}

/**
 * @brief Read information for one PID into the process structure.
 *
 * @param kvm_descriptor Open kvm descriptor.
 * @param pid PID to query.
 * @param proc Structure to populate.
 * @param read_cmd Non-zero to read argv[0].
 * @return 0 on success, -1 on failure.
 */
static int read_process_info(kvm_t *kvm_descriptor, pid_t pid,
                             struct process *proc, int read_cmd) {
    /* kvm_getprocs() leaves count untouched on failure. */
    int count = 0;
    struct kinfo_proc *kproc =
        kvm_getprocs(kvm_descriptor, KERN_PROC_PID, pid, &count);
    if (count == 0 || kproc == NULL || (kproc->ki_flag & P_SYSTEM) ||
        (kproc->ki_stat == SZOMB) ||
        kinfo_proc_to_proc(kvm_descriptor, kproc, proc, read_cmd) != 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief Read a parent PID through an existing kvm descriptor.
 *
 * @param kvm_descriptor Open kvm descriptor.
 * @param pid PID to query.
 * @return The parent PID, or -1 on failure.
 */
static pid_t getppid_via_kvm(kvm_t *kvm_descriptor, pid_t pid) {
    int count = 0;
    const struct kinfo_proc *kproc;
    if (pid <= 0) {
        return (pid_t)(-1);
    }
    kproc = kvm_getprocs(kvm_descriptor, KERN_PROC_PID, pid, &count);
    if (count == 0 || kproc == NULL) {
        return (pid_t)(-1);
    }
    /* Exclude zombies from the ancestry walk. */
    if (kproc->ki_stat == SZOMB) {
        return (pid_t)(-1);
    }
    return kproc->ki_ppid;
}

/**
 * @brief Read the parent PID of a process.
 *
 * @param pid PID to query.
 * @return The parent PID, or -1 when the process is gone, a zombie, or the
 *         platform lookup fails.
 *
 * @note getppid_of(1) differs: Linux returns -1, FreeBSD 0, macOS 0 or -1.
 */
pid_t getppid_of(pid_t pid) {
    pid_t ppid;
    kvm_t *kvm_descriptor;
    kvm_descriptor = open_kvm();
    if (kvm_descriptor == NULL) {
        return (pid_t)(-1);
    }
    ppid = getppid_via_kvm(kvm_descriptor, pid);
    if (kvm_close(kvm_descriptor) != 0) {
        perror("kvm_close");
    }
    return ppid;
}

/**
 * @brief Report whether a process is currently stopped.
 *
 * @param pid PID to query.
 * @return 1 when stopped, 0 when running, gone or unreadable.
 *
 * @note Read-only: sends no signal and starts no scan.
 */
int process_is_stopped(pid_t pid) {
    int count = 0;
    int stopped = 0;
    kvm_t *kvm_descriptor;
    const struct kinfo_proc *kproc;

    if (pid <= 0) {
        return 0;
    }
    kvm_descriptor = open_kvm();
    if (kvm_descriptor == NULL) {
        return 0;
    }
    kproc = kvm_getprocs(kvm_descriptor, KERN_PROC_PID, pid, &count);
    if (kproc != NULL && count > 0) {
        stopped = (kproc->ki_stat == SSTOP);
    }
    if (kvm_close(kvm_descriptor) != 0) {
        perror("kvm_close");
    }
    return stopped;
}

/**
 * @brief Read one process's parent through the walk's kvm descriptor.
 *
 * @param context Open kvm descriptor.
 * @param pid PID whose parent is wanted.
 * @return The parent PID, or -1 on failure.
 */
static pid_t is_child_lookup_via_kvm(void *context, pid_t pid) {
    return getppid_via_kvm((kvm_t *)context, pid);
}

/**
 * @brief Determine whether one process descends from another.
 *
 * @param child_pid PID to test.
 * @param parent_pid PID of the potential ancestor.
 * @return 1 when parent_pid is in the ancestry chain, 0 otherwise.
 *
 * Walks the shared parent chain with the platform's lookup. Returns 0 for
 * an invalid or self pair, a chain longer than IS_CHILD_MAX_DEPTH, or two
 * consecutive lookup failures.
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    int ret;
    kvm_t *kvm_descriptor;
    kvm_descriptor = open_kvm();
    if (kvm_descriptor == NULL) {
        return 0;
    }
    ret = is_child_of_via(is_child_lookup_via_kvm, kvm_descriptor, child_pid,
                          parent_pid);
    if (kvm_close(kvm_descriptor) != 0) {
        perror("kvm_close");
    }
    return ret;
}

/**
 * @brief Retrieve the next process matching the filter.
 *
 * @param iter Iterator to advance.
 * @param proc Structure to populate.
 * @return 0 with data in proc, -1 when the scan ends or an argument is
 *         NULL; the Linux backend also returns PROCESS_ITER_ERROR when a
 *         scan stops early on an error.
 *
 * Always populates pid, ppid, cpu_time and start_time; command is filled
 * only when filter->read_cmd is set. Zombies and system processes are
 * skipped where the platform can identify them.
 */
int get_next_process(struct process_iterator *iter, struct process *proc) {
    if (iter == NULL || proc == NULL || iter->filter == NULL) {
        return -1;
    }

    if (iter->current_index >= iter->proc_count) {
        return -1;
    }

    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        if (read_process_info(iter->kvm_descriptor, iter->filter->pid, proc,
                              iter->filter->read_cmd) != 0) {
            iter->current_index = iter->proc_count = 0;
            return -1;
        }
        iter->current_index = iter->proc_count = 1;
        return 0;
    }

    while (iter->current_index < iter->proc_count) {
        struct kinfo_proc *kproc = &iter->kinfo_procs[iter->current_index++];
        if ((kproc->ki_flag & P_SYSTEM) || (kproc->ki_stat == SZOMB)) {
            continue;
        }
        /* Filter before the expensive conversion and argv[0] read. */
        if (!process_matches_filter(kproc->ki_pid, iter->filter)) {
            continue;
        }

        if (kinfo_proc_to_proc(iter->kvm_descriptor, kproc, proc,
                               iter->filter->read_cmd) != 0) {
            continue;
        }

        return 0;
    }

    return -1;
}

/**
 * @brief Release the iterator's platform-specific resources.
 *
 * @param iter Iterator to close.
 * @return 0 on success, -1 on a close failure; macOS has none to fail.
 *
 * @note Re-initialize the iterator before using it again.
 */
int close_process_iterator(struct process_iterator *iter) {
    int ret = 0;
    if (iter == NULL) {
        return -1;
    }
    free(iter->kinfo_procs);
    if (iter->kvm_descriptor != NULL && kvm_close(iter->kvm_descriptor) != 0) {
        perror("kvm_close");
        ret = -1;
    }
    memset(iter, 0, sizeof(*iter));
    return ret;
}

#endif
#else
/* Keep the translation unit non-empty on other platforms. */
typedef int cpulimit_process_iterator_freebsd_placeholder;
#endif
