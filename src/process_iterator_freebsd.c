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
#define inline __inline
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
 * @brief Open the kvm(3) interface for process information access
 * @return Opened kvm descriptor on success, NULL on failure
 *
 * Allocates a temporary error buffer, opens the kvm interface in read-only
 * mode, and frees the buffer. On allocation failure, exits the program.
 * On kvm_openfiles() failure, prints an error and returns NULL.
 *
 * Callers must close the returned descriptor with kvm_close().
 */
static kvm_t *open_kvm(void) {
    kvm_t *kvm_descriptor;
    char *errbuf;
    /* Allocate error buffer for kvm interface */
    errbuf = (char *)malloc(sizeof(char) * _POSIX2_LINE_MAX);
    if (errbuf == NULL) {
        fprintf(stderr, "Memory allocation failed for the error buffer\n");
        exit(EXIT_FAILURE);
    }
    kvm_descriptor = kvm_openfiles(NULL, _PATH_DEVNULL, NULL, O_RDONLY, errbuf);
    if (kvm_descriptor == NULL) {
        fprintf(stderr, "kvm_openfiles: %s\n", errbuf);
    }
    free(errbuf);
    return kvm_descriptor;
}

/**
 * @brief Initialize a process iterator with specified filter criteria
 * @param iter Pointer to the process_iterator structure to initialize
 * @param filter Pointer to filter criteria, must remain valid during iteration
 * @return 0 on success, -1 on failure (including NULL iter or filter);
 *         may call exit() on fatal errors
 *         (e.g., out-of-memory)
 *
 * This function prepares the iterator for process enumeration. The behavior
 * varies by platform:
 * - Linux: Opens /proc directory, may skip if filtering single process
 * - FreeBSD: Opens kvm descriptor, retrieves process snapshot if needed
 * - macOS: Retrieves process ID list snapshot, may skip if filtering single
 *          process
 *
 * The filter pointer is stored and must remain valid until
 * close_process_iterator() is called.
 */
int init_process_iterator(struct process_iterator *iter,
                          const struct process_filter *filter) {
    const struct kinfo_proc *proc_snapshot;

    if (iter == NULL || filter == NULL) {
        return -1;
    }
    iter->filter = filter;

    /*
     * Open kvm(3) interface to access kernel virtual memory and
     * process information
     */
    iter->kvm_descriptor = open_kvm();
    if (iter->kvm_descriptor == NULL) {
        return -1;
    }

    /* Optimization for single process without children */
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        /*
         * Skip retrieving full process list when querying a single
         * process; get_next_process() will use kvm_getprocs() directly
         */
        iter->kinfo_procs = NULL;
        iter->current_index = 0;
        iter->proc_count = 1;
        return 0;
    }

    /*
     * Retrieve snapshot of all processes via KERN_PROC_PROC.
     * This returns a pointer to kernel data that must be copied.
     */
    proc_snapshot = kvm_getprocs(iter->kvm_descriptor, KERN_PROC_PROC, 0,
                                 &iter->proc_count);
    if (proc_snapshot == NULL) {
        kvm_close(iter->kvm_descriptor);
        return -1;
    }
    /* Copy process list to iterator's own memory */
    iter->kinfo_procs = (struct kinfo_proc *)malloc(sizeof(struct kinfo_proc) *
                                                    (size_t)iter->proc_count);
    if (iter->kinfo_procs == NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        close_process_iterator(iter);
        return -1;
    }
    memcpy(iter->kinfo_procs, proc_snapshot,
           sizeof(struct kinfo_proc) * (size_t)iter->proc_count);
    iter->current_index = 0;
    return 0;
}

/**
 * @brief Convert FreeBSD kinfo_proc structure to portable process structure
 * @param kvm_descriptor Kernel virtual memory descriptor for kvm_getargv()
 * @param kproc Pointer to source kinfo_proc structure
 * @param proc Pointer to destination process structure to populate
 * @param read_cmd Whether to read command path (0=skip, 1=read)
 * @return 0 on success, -1 on failure
 *
 * Extracts essential process information from FreeBSD's kinfo_proc and
 * converts it to the platform-independent process structure. The ki_runtime
 * field (in microseconds) is converted to milliseconds for cpu_time.
 *
 * When read_cmd is set, retrieves command arguments via kvm_getargv()
 * and uses the first argument (argv[0]) as the command path.
 */
static int kinfo_proc_to_proc(kvm_t *kvm_descriptor, struct kinfo_proc *kproc,
                              struct process *proc, int read_cmd) {
    char **args;
    size_t len_max;
    if (kproc == NULL || proc == NULL) {
        return -1;
    }
    memset(proc, 0, sizeof(struct process));
    proc->pid = kproc->ki_pid;
    proc->ppid = kproc->ki_ppid;
    /* Convert runtime from microseconds to milliseconds */
    proc->cpu_time = (double)kproc->ki_runtime / 1000.0;
    if (!read_cmd) {
        return 0;
    }
    len_max = sizeof(proc->command) - 1;
    /* Retrieve command arguments as string array */
    args = kvm_getargv(kvm_descriptor, kproc, (int)len_max);
    if (args == NULL || args[0] == NULL) {
        return -1;
    }
    strncpy(proc->command, args[0], len_max);
    proc->command[len_max] = '\0';
    /*
     * Reject processes with empty command names (e.g. execve with
     * argv[0]=="")
     */
    if (proc->command[0] == '\0') {
        return -1;
    }
    return 0;
}

/**
 * @brief Retrieve information for a single process by PID
 * @param kvm_descriptor Kernel virtual memory descriptor
 * @param pid Process ID to query
 * @param proc Pointer to process structure to populate
 * @param read_cmd Whether to read command path (0=skip, 1=read)
 * @return 0 on success, -1 on failure or if process not found
 *
 * Uses kvm_getprocs() with KERN_PROC_PID to query a specific process.
 * Returns failure if the process doesn't exist, is a zombie, is a kernel
 * thread, or if conversion fails.
 */
static int read_process_info(kvm_t *kvm_descriptor, pid_t pid,
                             struct process *proc, int read_cmd) {
    int count;
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
 * @brief Internal helper to get parent process ID without opening kvm
 * @param kvm_descriptor Kernel virtual memory descriptor (must be already open)
 * @param pid Process ID to query
 * @return Parent process ID on success, -1 on error or if process not found
 *
 * Uses an existing kvm descriptor to query PPID. This avoids the overhead
 * of repeatedly opening and closing kvm when checking multiple processes.
 */
static pid_t getppid_via_kvm(kvm_t *kvm_descriptor, pid_t pid) {
    int count;
    struct kinfo_proc *kproc;
    if (pid <= 0) {
        return (pid_t)(-1);
    }
    kproc = kvm_getprocs(kvm_descriptor, KERN_PROC_PID, pid, &count);
    return (count == 0 || kproc == NULL) ? (pid_t)(-1) : kproc->ki_ppid;
}

/**
 * @brief Retrieve the parent process ID for a given process
 * @param pid Process ID to query
 * @return Parent process ID on success, -1 on error
 *
 * Queries the system to determine the parent process ID of the specified
 * process. Implementation varies by platform:
 * - Linux: Parses /proc/[pid]/stat for PPID field
 * - FreeBSD: Uses kvm_getprocs() with KERN_PROC_PID
 * - macOS: Uses proc_pidinfo() with PROC_PIDTASKALLINFO
 *
 * Returns -1 if the process does not exist, is a zombie, or if system
 * call fails.
 */
pid_t getppid_of(pid_t pid) {
    pid_t ppid;
    kvm_t *kvm_descriptor;
    kvm_descriptor = open_kvm();
    if (kvm_descriptor == NULL) {
        return (pid_t)(-1);
    }
    ppid = getppid_via_kvm(kvm_descriptor, pid);
    kvm_close(kvm_descriptor);
    return ppid;
}

/**
 * @brief Internal helper to check parent-child relationship with open kvm
 * @param kvm_descriptor Kernel virtual memory descriptor (must be already open)
 * @param child_pid Process ID to check for descendant relationship
 * @param parent_pid Process ID of the potential ancestor
 * @return 1 if child_pid is a descendant of parent_pid, 0 otherwise
 *
 * Traverses the parent chain from child_pid up to init (PID 1) to determine
 * if parent_pid appears in the ancestry. Uses an existing kvm descriptor to
 * avoid repeated open/close overhead.
 */
static int is_child_via_kvm(kvm_t *kvm_descriptor, pid_t child_pid,
                            pid_t parent_pid) {
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    /*
     * Fast-path: any existing non-init process is ultimately a child of init
     * (PID 1)
     */
    if (parent_pid == 1) {
        return getppid_via_kvm(kvm_descriptor, child_pid) != (pid_t)(-1);
    }
    /* Walk up the parent chain looking for parent_pid */
    while (child_pid > 1 && child_pid != parent_pid) {
        child_pid = getppid_via_kvm(kvm_descriptor, child_pid);
    }
    return child_pid == parent_pid;
}

/**
 * @brief Determine if one process is a descendant of another
 * @param child_pid Process ID to check for descendant relationship
 * @param parent_pid Process ID of the potential ancestor
 * @return 1 if child_pid is a descendant of parent_pid, 0 otherwise
 *
 * Traverses the parent chain from child_pid up to the init process (PID 1),
 * checking if parent_pid is encountered. Returns 1 if parent_pid is found
 * in the ancestry chain, indicating child_pid is a descendant (child,
 * grandchild, etc.) of parent_pid.
 *
 * Special cases:
 * - Returns 0 if child_pid <= 1, parent_pid <= 0, or child_pid == parent_pid
 * - Returns 1 for parent_pid == 1 only when child_pid exists and is not init
 * - Linux: Uses process start times to handle PID reuse
 * - FreeBSD/macOS: Relies on current process hierarchy only
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    int ret;
    kvm_t *kvm_descriptor;
    kvm_descriptor = open_kvm();
    if (kvm_descriptor == NULL) {
        return 0;
    }
    ret = is_child_via_kvm(kvm_descriptor, child_pid, parent_pid);
    kvm_close(kvm_descriptor);
    return ret;
}

/**
 * @brief Retrieve the next process matching the filter criteria
 * @param iter Pointer to the process_iterator structure
 * @param proc Pointer to process structure to populate with process information
 * @return 0 on success with process data in proc, -1 if no more processes
 *
 * Advances the iterator to the next process that satisfies the filter
 * criteria. The process structure is populated with information based on
 * the filter's read_cmd flag:
 * - Always populated: pid, ppid, cpu_time
 * - Conditionally populated: command (only if filter->read_cmd is set)
 *
 * This function skips zombie processes, system processes (on FreeBSD/macOS),
 * and processes not matching the PID filter criteria.
 */
int get_next_process(struct process_iterator *iter, struct process *proc) {
    if (iter == NULL || proc == NULL || iter->filter == NULL) {
        return -1;
    }

    if (iter->current_index >= iter->proc_count) {
        return -1;
    }

    /* Handle single process without children */
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        if (read_process_info(iter->kvm_descriptor, iter->filter->pid, proc,
                              iter->filter->read_cmd) != 0) {
            iter->current_index = iter->proc_count = 0;
            return -1;
        }
        iter->current_index = iter->proc_count = 1;
        return 0;
    }

    /* Iterate through process snapshot */
    while (iter->current_index < iter->proc_count) {
        struct kinfo_proc *kproc = &iter->kinfo_procs[iter->current_index++];
        /* Skip kernel threads and zombie processes */
        if ((kproc->ki_flag & P_SYSTEM) || (kproc->ki_stat == SZOMB)) {
            continue;
        }
        /*
         * Apply PID filter early using kinfo_proc data before
         * performing expensive conversion and command reading
         */
        if (iter->filter->pid != 0 && iter->filter->include_children &&
            kproc->ki_pid != iter->filter->pid &&
            !is_child_via_kvm(iter->kvm_descriptor, kproc->ki_pid,
                              iter->filter->pid)) {
            continue;
        }

        /* Convert to platform-independent structure */
        if (kinfo_proc_to_proc(iter->kvm_descriptor, kproc, proc,
                               iter->filter->read_cmd) != 0) {
            continue;
        }

        return 0;
    }

    return -1;
}

/**
 * @brief Close the process iterator and release allocated resources
 * @param iter Pointer to the process_iterator structure to close
 * @return 0 on success, -1 on failure
 *
 * Releases platform-specific resources allocated during initialization:
 * - Linux: Closes /proc directory stream
 * - FreeBSD: Frees process array and closes kvm descriptor
 * - macOS: Frees process ID list
 *
 * After this call, the iterator must not be used until re-initialized.
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

#endif /* CPULIMIT_PROCESS_ITERATOR_FREEBSD_C */
#endif /* __FreeBSD__ */
