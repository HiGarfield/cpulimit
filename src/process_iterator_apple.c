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
#include <limits.h>
#include <mach/mach_time.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)(-1))
#endif

/**
 * @brief Initialize a process iterator with specified filter criteria
 * @param iter Pointer to the process_iterator structure to initialize
 * @param filter Pointer to filter criteria, must remain valid during iteration
 * @return 0 on success, -1 on failure (including NULL iter or filter);
 *         may call exit() on fatal errors (e.g., out-of-memory)
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
        iter->proc_count = 1;
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

        iter->pid_list = (pid_t *)malloc((size_t)buffer_size);
        if (iter->pid_list == NULL) {
            break;
        }

        /* Populate buffer with process IDs */
        bytes = proc_listpids(PROC_ALL_PIDS, 0, iter->pid_list, buffer_size);
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
            iter->proc_count = bytes / (int)sizeof(pid_t);
            success = 1;
            break;
        }

        /* Buffer too small - free and retry with larger size */
        free(iter->pid_list);
        iter->pid_list = NULL;
        if (buffer_size > INT_MAX / 2) {
            break;
        }
        buffer_size *= 2;
    }

    /* Fatal error if all retries exhausted or allocation failed */
    if (!success) {
        close_process_iterator(iter);
        return -1;
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
 * The result is in milliseconds to match the cpu_time field of the
 * process structure.
 */
static double platform_time_to_ms(double platform_time) {
    static int initialized = 0;
    static double timebase_factor = 1.0; /* safe default: 1 ns per tick */
    if (!initialized) {
        mach_timebase_info_data_t timebase_info;
        /*
         * mach_timebase_info() is documented to always succeed on Apple
         * platforms.  Check the return value defensively and keep the
         * safe default (1.0) if it ever fails.
         */
        if (mach_timebase_info(&timebase_info) == KERN_SUCCESS) {
            /* Convert to milliseconds: (numer/denom) gives ns per tick */
            timebase_factor =
                (double)timebase_info.numer / (double)timebase_info.denom;
        }
        initialized = 1;
    }
    return platform_time * timebase_factor / 1e6;
}

/**
 * @brief Retrieve argv[0] for a process
 * @param pid Process ID to query
 * @param buf Buffer to store argv[0]
 * @param bufsize Size of the buffer in bytes (must be > 0)
 * @return 0 on success, -1 on failure
 *
 * The KERN_PROCARGS2 buffer layout:
 *   [argc (int)][exec_path\0][padding \0s][argv[0]\0][argv[1]\0]...
 *
 * exec_path is the fully resolved executable path (symlinks resolved).
 * argv[0] is the string passed to execve() by the caller.
 * This function skips exec_path and its padding to locate the true argv[0].
 * Returns -1 if argv[0] is empty, the process does not exist, or the buffer
 * is too small.
 */
static int get_proc_argv0(pid_t pid, char *buf, size_t bufsize) {
    int mib[3] = {CTL_KERN, KERN_PROCARGS2},
        mib_argmax[2] = {CTL_KERN, KERN_ARGMAX}, argmax;
    size_t size, argv0_len;
    char *procargs;
    const char *sp, *end, *nul;

    if (buf == NULL || bufsize == 0) {
        return -1;
    }

    mib[2] = (int)pid;

    /* Get the maximum argument size */
    size = sizeof(argmax);
    if (sysctl(mib_argmax, 2, &argmax, &size, NULL, 0) != 0 || argmax <= 0) {
        return -1;
    }

    /*
     * Allocate exactly KERN_ARGMAX bytes.  The kernel stores at most
     * ARG_MAX bytes of process argument data, and KERN_ARGMAX returns
     * that same ARG_MAX value, so this single allocation is always
     * large enough: KERN_PROCARGS2 will never fail with ENOMEM when
     * the buffer is KERN_ARGMAX bytes.  No retry is needed.
     */
    procargs = (char *)malloc((size_t)argmax);
    if (procargs == NULL) {
        return -1;
    }

    size = (size_t)argmax;
    if (sysctl(mib, 3, procargs, &size, NULL, 0) != 0) {
        free(procargs);
        return -1;
    }

    sp = procargs + sizeof(int); /* skip argc field */
    end = procargs + size;

    /* Skip exec_path */
    while (sp < end && *sp != '\0') {
        sp++;
    }
    while (sp < end && *sp == '\0') {
        sp++;
    }

    if (sp >= end || *sp == '\0') {
        free(procargs);
        return -1; /* argv[0] missing */
    }

    /*
     * Verify argv[0] is NUL-terminated within the valid sysctl payload
     * [sp, end) before copying.  If no NUL exists, the kernel data is
     * malformed; return -1 rather than reading past the payload.
     * Also return -1 when argv[0] is too long to fit in buf: the caller
     * relies on getting a complete, untruncated name for process matching.
     */
    nul = sp;

    while (nul < end && *nul != '\0') {
        nul++;
    }
    if (nul >= end) {
        free(procargs);
        return -1; /* no NUL terminator within valid payload */
    }
    argv0_len = (size_t)(nul - sp);
    if (argv0_len >= bufsize) {
        free(procargs);
        return -1; /* argv[0] too long for caller's buffer */
    }
    memcpy(buf, sp, argv0_len);
    buf[argv0_len] = '\0';

    free(procargs);
    return 0;
}

/**
 * @brief Convert macOS proc_taskallinfo to portable process structure
 * @param task_info Pointer to source proc_taskallinfo structure
 * @param proc Pointer to destination process structure to populate
 * @param read_cmd Whether to read command path (0=skip, 1=read)
 * @return 0 on success, -1 on failure
 *
 * Extracts process information from macOS-specific proc_taskallinfo and
 * converts it to the platform-independent process structure. CPU time is
 * calculated as the sum of user and system time, converted to milliseconds.
 *
 * When read_cmd is set, retrieves the executable path via sysctl.
 */
static int proc_taskinfo_to_proc(struct proc_taskallinfo *task_info,
                                 struct process *proc, int read_cmd) {
    proc->pid = (pid_t)task_info->pbsd.pbi_pid;
    proc->ppid = (pid_t)task_info->pbsd.pbi_ppid;
    /* Sum user and system CPU time, convert to milliseconds */
    proc->cpu_time =
        platform_time_to_ms((double)task_info->ptinfo.pti_total_user) +
        platform_time_to_ms((double)task_info->ptinfo.pti_total_system);
    if (!read_cmd) {
        return 0;
    }

    if (get_proc_argv0(proc->pid, proc->command, sizeof(proc->command)) != 0) {
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
 * - Returns 1 for parent_pid == 1 only when getppid_of(child_pid) succeeds
 *   (that is, child_pid is non-init, non-zombie, and has a valid PPID)
 * - Returns 0 if the parent chain exceeds IS_CHILD_MAX_DEPTH steps (guards
 *   against infinite loops caused by PID reuse cycles)
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    int depth = 0;
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
        pid_t next_ppid;
        /*
         * Depth limit guards against PID reuse cycles of length >= 2.
         * Example: A has ppid=B and B has ppid=A. The self-parenting check
         * (next_ppid == child_pid) only catches length-1 cycles; without
         * this counter the loop would never terminate on length-2+ cycles.
         */
        if (depth++ >= IS_CHILD_MAX_DEPTH) {
            return 0;
        }
        next_ppid = getppid_of(child_pid);
        /*
         * Guard against invalid parent links or self-parenting processes,
         * either of which would cause an infinite loop.
         */
        if (next_ppid <= 0 || next_ppid == child_pid) {
            return 0;
        }
        child_pid = next_ppid;
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
 * and converts it to the platform-independent process structure via
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
 * @return 0 on success with process data in proc, -1 if no more processes or
 *         if iter, proc, or iter->filter is NULL
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
        if (read_process_info(iter->filter->pid, proc,
                              iter->filter->read_cmd) != 0) {
            iter->current_index = iter->proc_count = 0;
            return -1;
        }
        iter->current_index = iter->proc_count = 1;
        return 0;
    }
    /* Iterate through process ID list, applying filters */
    while (iter->current_index < iter->proc_count) {
        if (read_process_info(iter->pid_list[iter->current_index], proc,
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
    free(iter->pid_list);
    memset(iter, 0, sizeof(*iter));
    return 0;
}

#endif /* CPULIMIT_PROCESS_ITERATOR_APPLE_C */
#else
/* Placeholder to avoid empty compilation unit on non-Apple platforms. */
typedef int cpulimit_process_iterator_apple_placeholder;
#endif /* __APPLE__ */
