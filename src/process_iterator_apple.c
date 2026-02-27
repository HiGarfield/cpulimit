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

#ifndef CPULIMIT_PROCESS_ITERATOR_APPLE_C
#define CPULIMIT_PROCESS_ITERATOR_APPLE_C

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
#include <sys/proc.h>
#include <sys/types.h>

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
    const int max_retries = 5, min_buffer_size = 1024 * (int)sizeof(pid_t);
    int buffer_size, retries, success = 0;

    if (iter == NULL || filter == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->filter = filter;

    /* Optimization: single process without children requires no snapshot */
    if (filter->pid != 0 && !filter->include_children) {
        /*
         * Skip retrieving process list; get_next_process() will
         * query the single process directly
         */
        iter->count = 1;
        return 0;
    }
    /*
     * Query required buffer size for process list.
     * proc_listpids() returns number of bytes needed.
     */
    buffer_size = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (buffer_size < min_buffer_size) {
        buffer_size = min_buffer_size;
    }
    /*
     * Retry loop to handle race conditions where process list changes
     * between size query and actual retrieval. Buffer is doubled on
     * each retry attempt.
     */
    for (retries = 0; retries < max_retries; retries++) {
        int bytes;

        if ((iter->pidlist = (pid_t *)malloc((size_t)buffer_size)) == NULL) {
            break;
        }

        /* Populate buffer with process IDs */
        bytes = proc_listpids(PROC_ALL_PIDS, 0, iter->pidlist, buffer_size);
        if (bytes <= 0) {
            /* System call failed - cannot recover */
            break;
        }
        /*
         * Success if returned bytes fit in buffer with room to spare.
         * Some slack prevents race conditions where processes spawn
         * between our allocation and the system call.
         */
        if (bytes < buffer_size) {
            iter->count = bytes / (int)sizeof(pid_t);
            success = 1;
            break;
        }

        /* Buffer too small - free and retry with larger size */
        free(iter->pidlist);
        iter->pidlist = NULL;
        buffer_size *= 2;
    }

    /* Fatal error if all retries exhausted or allocation failed */
    if (!success) {
        if (iter->pidlist != NULL) {
            free(iter->pidlist);
            iter->pidlist = NULL;
        }
        exit(EXIT_FAILURE);
    }

    return 0;
}

/**
 * @brief Convert Mach time units to milliseconds
 * @param platform_time Time value in Mach absolute time units
 * @return Time in milliseconds
 *
 * Mach uses platform-dependent time units that must be converted using
 * the timebase ratio (numer/denom) from mach_timebase_info(). This
 * function caches the conversion factor on first call.
 *
 * The result is in milliseconds to match the cputime field of the
 * process structure.
 */
static double platform_time_to_ms(double platform_time) {
    static double timebase_factor = -1;
    if (timebase_factor < 0) {
        mach_timebase_info_data_t timebase_info;
        mach_timebase_info(&timebase_info);
        /* Convert to milliseconds: (numer/denom) gives nanoseconds per tick */
        timebase_factor =
            (double)timebase_info.numer / (double)timebase_info.denom;
    }
    return platform_time * timebase_factor / 1e6;
}

/**
 * @brief Convert macOS proc_taskallinfo to portable process structure
 * @param task_info Pointer to source proc_taskallinfo structure
 * @param proc Pointer to destination process structure to populate
 * @param read_cmd Whether to read command path (0=skip, 1=read)
 * @return 0 on success, -1 on failure
 *
 * Extracts process information from macOS-specific proc_taskallinfo and
 * converts iter to the platform-independent process structure. CPU time is
 * calculated as the sum of user and system time, converted to milliseconds.
 *
 * When read_cmd is set, retrieves the executable path via proc_pidpath().
 */
static int proc_taskinfo_to_proc(struct proc_taskallinfo *task_info,
                                 struct process *proc, int read_cmd) {
    proc->pid = (pid_t)task_info->pbsd.pbi_pid;
    proc->ppid = (pid_t)task_info->pbsd.pbi_ppid;
    /* Sum user and system CPU time, convert to milliseconds */
    proc->cputime =
        platform_time_to_ms((double)task_info->ptinfo.pti_total_user) +
        platform_time_to_ms((double)task_info->ptinfo.pti_total_system);
    if (!read_cmd) {
        return 0;
    }
    /* Retrieve full path to executable */
    if (proc_pidpath(proc->pid, proc->command, sizeof(proc->command)) <= 0) {
        return -1;
    }
    proc->command[sizeof(proc->command) - 1] = '\0';
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
 * @brief Retrieve detailed task information for a process
 * @param pid Process ID to query
 * @param task_info Pointer to structure to populate with task information
 * @return 0 on success, -1 on failure or if process should be skipped
 *
 * Uses proc_pidinfo() with PROC_PIDTASKALLINFO to retrieve comprehensive
 * process information including BSD process info and task statistics.
 *
 * Returns -1 for:
 * - Permission denied (EPERM) or process not found (ESRCH) - silently
 * - Other errors - prints error message
 * - Zombie processes (pbi_status == SZOMB)
 * - System processes (pbi_flags & PROC_FLAG_SYSTEM)
 */
static int get_proc_taskinfo(pid_t pid, struct proc_taskallinfo *task_info) {
    if (proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, task_info,
                     sizeof(*task_info)) != (int)sizeof(*task_info)) {
        /* Silently skip common errors for processes we cannot access */
        if (errno != EPERM && errno != ESRCH) {
            perror("proc_pidinfo");
        }
        return -1;
    }
    if (task_info->pbsd.pbi_status == SZOMB) {
        return -1;
    }
    if (task_info->pbsd.pbi_flags & PROC_FLAG_SYSTEM) {
        return -1;
    }
    return 0;
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
    struct proc_taskallinfo task_info;
    if (get_proc_taskinfo(pid, &task_info) == 0) {
        return (pid_t)task_info.pbsd.pbi_ppid;
    }
    return (pid_t)(-1);
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
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    /*
     * Fast-path: any existing non-init process is ultimately a child of init
     * (PID 1)
     */
    if (parent_pid == 1) {
        return getppid_of(child_pid) != (pid_t)(-1);
    }
    /* Walk up the parent chain looking for parent_pid */
    while (child_pid > 1 && child_pid != parent_pid) {
        child_pid = getppid_of(child_pid);
    }
    return child_pid == parent_pid;
}

/**
 * @brief Retrieve process information for a specific PID
 * @param pid Process ID to query
 * @param proc Pointer to process structure to populate
 * @param read_cmd Whether to read command path (0=skip, 1=read)
 * @return 0 on success, -1 on failure
 *
 * Internal helper that retrieves task information via get_proc_taskinfo()
 * and converts iter to the platform-independent process structure via
 * proc_taskinfo_to_proc(). Used by get_next_process() for both
 * single-process and multi-process iteration.
 */
static int read_process_info(pid_t pid, struct process *proc, int read_cmd) {
    struct proc_taskallinfo task_info;
    memset(proc, 0, sizeof(struct process));
    if (get_proc_taskinfo(pid, &task_info) != 0) {
        return -1;
    }
    if (proc_taskinfo_to_proc(&task_info, proc, read_cmd) != 0) {
        return -1;
    }
    return 0;
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
 * - Always populated: pid, ppid, cputime
 * - Conditionally populated: command (only if filter->read_cmd is set)
 *
 * This function skips zombie processes, system processes (on FreeBSD/macOS),
 * and processes not matching the PID filter criteria.
 */
int get_next_process(struct process_iterator *iter, struct process *proc) {
    if (iter == NULL || proc == NULL || iter->filter == NULL) {
        return -1;
    }
    if (iter->current_index >= iter->count) {
        return -1;
    }
    /* Handle single process without children */
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        if (read_process_info(iter->filter->pid, proc,
                              iter->filter->read_cmd) == 0) {
            iter->current_index = iter->count = 1;
            return 0;
        }
        iter->current_index = iter->count = 0;
        return -1;
    }
    /* Iterate through process ID list, applying filters */
    while (iter->current_index < iter->count) {
        if (read_process_info(iter->pidlist[iter->current_index], proc,
                              iter->filter->read_cmd) == 0) {
            /*
             * Apply PID filter after reading process info.
             * Accept if: no filter, exact match, or descendant match.
             */
            if (iter->filter->pid == 0 || proc->pid == iter->filter->pid ||
                (iter->filter->include_children &&
                 is_child_of(proc->pid, iter->filter->pid))) {
                iter->current_index++;
                return 0;
            }
        }
        iter->current_index++;
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
    if (iter == NULL) {
        return -1;
    }
    if (iter->pidlist != NULL) {
        free(iter->pidlist);
        iter->pidlist = NULL;
    }
    iter->filter = NULL;
    iter->count = 0;
    iter->current_index = 0;
    return 0;
}

#endif /* CPULIMIT_PROCESS_ITERATOR_APPLE_C */
#endif /* __APPLE__ */
