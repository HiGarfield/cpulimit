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
 * Author of this macOS process iterator: Simon Sigurdhsson
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
    const int max_retries = 5, min_buffer_size = 1024 * (int)sizeof(pid_t);
    int buffer_size, retries, success = 0;

    if (iter == NULL || filter == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->filter = filter;

    /* A single PID without children needs no snapshot. */
    if (filter->pid != 0 && !filter->include_children) {
        iter->proc_count = 1;
        return 0;
    }
    buffer_size = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (buffer_size < min_buffer_size) {
        buffer_size = min_buffer_size;
    }
    /* Retry with a larger buffer if the table changed between calls. */
    for (retries = 0; retries < max_retries; retries++) {
        int bytes;

        iter->pid_list = (pid_t *)malloc((size_t)buffer_size);
        if (iter->pid_list == NULL) {
            break;
        }

        bytes = proc_listpids(PROC_ALL_PIDS, 0, iter->pid_list, buffer_size);
        if (bytes <= 0) {
            break;
        }
        if (bytes < buffer_size) {
            iter->proc_count = bytes / (int)sizeof(pid_t);
            success = 1;
            break;
        }

        free(iter->pid_list);
        iter->pid_list = NULL;
        if (buffer_size > INT_MAX / 2) {
            break;
        }
        buffer_size *= 2;
    }

    if (!success) {
        close_process_iterator(iter);
        return -1;
    }

    return 0;
}

/**
 * @brief Convert Mach absolute time to milliseconds.
 *
 * @param platform_time Time in Mach ticks.
 * @return Time in milliseconds.
 *
 * @note Caches the timebase ratio from mach_timebase_info().
 */
static double platform_time_to_ms(double platform_time) {
    static int initialized = 0;
    static double timebase_factor = 1.0; /* Default: 1 ns per tick. */
    if (!initialized) {
        mach_timebase_info_data_t timebase_info;
        if (mach_timebase_info(&timebase_info) == KERN_SUCCESS) {
            timebase_factor =
                (double)timebase_info.numer / (double)timebase_info.denom;
        }
        initialized = 1;
    }
    return platform_time * timebase_factor / 1e6;
}

/**
 * @brief Read argv[0] of a process through KERN_PROCARGS2.
 *
 * @param pid PID to query.
 * @param buf Destination buffer.
 * @param bufsize Size of buf, must be positive.
 * @return 0 on success, -1 when the payload cannot be parsed.
 *
 * @note A long argv[0] is truncated rather than rejected.
 */
static int get_proc_argv0(pid_t pid, char *buf, size_t bufsize) {
    int mib[3] = {CTL_KERN, KERN_PROCARGS2},
        mib_argmax[2] = {CTL_KERN, KERN_ARGMAX}, argmax = 0;
    size_t size, payload_size, argv0_len;
    char *procargs;
    const char *sp, *end, *nul;

    if (buf == NULL || bufsize == 0) {
        return -1;
    }

    mib[2] = (int)pid;

    size = sizeof(argmax);
    /* sysctl() reports the bytes written; guard against a short read. */
    if (sysctl(mib_argmax, 2, &argmax, &size, NULL, 0) != 0 ||
        size != sizeof(argmax) || argmax <= 0) {
        return -1;
    }

    /* Reserve sizeof(int) for argc in KERN_PROCARGS2. */
    payload_size = (size_t)argmax + sizeof(int);
    procargs = (char *)malloc(payload_size);
    if (procargs == NULL) {
        return -1;
    }

    size = payload_size;
    if (sysctl(mib, 3, procargs, &size, NULL, 0) != 0) {
        free(procargs);
        return -1;
    }
    /* Never scan past the bytes sysctl() reported. */
    if (size > payload_size) {
        free(procargs);
        return -1;
    }

    sp = procargs + sizeof(int);
    end = procargs + size;

    while (sp < end && *sp != '\0') {
        sp++;
    }
    while (sp < end && *sp == '\0') {
        sp++;
    }

    if (sp >= end || *sp == '\0') {
        free(procargs);
        return -1;
    }

    /* Require a NUL-terminated argv[0] inside the payload. */
    nul = sp;

    while (nul < end && *nul != '\0') {
        nul++;
    }
    if (nul >= end) {
        free(procargs);
        return -1;
    }
    argv0_len = (size_t)(nul - sp);
    if (argv0_len >= bufsize) {
        /* Truncate a long argv[0] instead of dropping the process. */
        argv0_len = bufsize - 1;
    }
    memcpy(buf, sp, argv0_len);
    buf[argv0_len] = '\0';

    free(procargs);
    return 0;
}

/**
 * @brief Convert proc_taskallinfo to the portable process structure.
 *
 * @param task_info Source structure.
 * @param proc Destination structure.
 * @param read_cmd Non-zero to read argv[0].
 * @return 0 on success, -1 on failure.
 */
static int proc_taskinfo_to_proc(struct proc_taskallinfo *task_info,
                                 struct process *proc, int read_cmd) {
    proc->pid = (pid_t)task_info->pbsd.pbi_pid;
    proc->ppid = (pid_t)task_info->pbsd.pbi_ppid;
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
 * @brief Read detailed task information for one PID.
 *
 * @param pid PID to query.
 * @param task_info Structure to populate.
 * @return 0 on success, -1 when the process must be skipped.
 */
static int get_proc_taskinfo(pid_t pid, struct proc_taskallinfo *task_info) {
    /* Clear errno: proc_pidinfo() need not set it on a short read. */
    errno = 0;
    if (proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, task_info,
                     sizeof(*task_info)) != (int)sizeof(*task_info)) {
        /* Only unexpected errors are worth printing during a full scan. */
        if (errno != 0 && errno != ESRCH && errno != EPERM) {
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
 * @brief Read the parent PID of a process.
 *
 * @param pid PID to query.
 * @return The parent PID, or -1 when the process is gone, a zombie, or the
 *         platform lookup fails.
 *
 * @note getppid_of(1) differs: Linux returns -1, FreeBSD 0, macOS 0 or -1.
 */
pid_t getppid_of(pid_t pid) {
    struct proc_taskallinfo *task_info;
    pid_t ppid;
    /* Heap-allocate: proc_taskallinfo exceeds the 256-byte stack limit. */
    task_info = (struct proc_taskallinfo *)malloc(sizeof(*task_info));
    if (task_info == NULL) {
        return (pid_t)(-1);
    }
    ppid = (get_proc_taskinfo(pid, task_info) == 0)
               ? (pid_t)task_info->pbsd.pbi_ppid
               : (pid_t)(-1);
    free(task_info);
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
    struct proc_taskallinfo *task_info;
    int stopped = 0;

    task_info = (struct proc_taskallinfo *)malloc(sizeof(*task_info));
    if (task_info == NULL) {
        return 0;
    }
    if (get_proc_taskinfo(pid, task_info) == 0) {
        stopped = (task_info->pbsd.pbi_status == SSTOP);
    }
    free(task_info);
    return stopped;
}

/**
 * @brief Read one process's parent for is_child_of_via().
 *
 * @param context Unused.
 * @param pid PID whose parent is wanted.
 * @return The parent PID, or -1 on failure.
 */
static pid_t is_child_lookup(void *context, pid_t pid) {
    (void)context;
    return getppid_of(pid);
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
    return is_child_of_via(is_child_lookup, NULL, child_pid, parent_pid);
}

/**
 * @brief Read information for one PID into the process structure.
 *
 * @param pid PID to query.
 * @param proc Structure to populate.
 * @param read_cmd Non-zero to read argv[0].
 * @return 0 on success, -1 on failure.
 */
static int read_process_info(pid_t pid, struct process *proc, int read_cmd) {
    struct proc_taskallinfo *task_info;
    int ret;
    memset(proc, 0, sizeof(struct process));
    task_info = (struct proc_taskallinfo *)malloc(sizeof(*task_info));
    if (task_info == NULL) {
        return -1;
    }
    if (get_proc_taskinfo(pid, task_info) != 0) {
        free(task_info);
        return -1;
    }
    ret = proc_taskinfo_to_proc(task_info, proc, read_cmd);
    if (ret == 0) {
        /* pbi_start_tvusec is microseconds; only equality matters. */
        proc->start_time = (double)task_info->pbsd.pbi_start_tvsec +
                           (double)task_info->pbsd.pbi_start_tvusec / 1e6;
    }
    free(task_info);
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
        if (read_process_info(iter->filter->pid, proc,
                              iter->filter->read_cmd) != 0) {
            iter->current_index = iter->proc_count = 0;
            return -1;
        }
        iter->current_index = iter->proc_count = 1;
        return 0;
    }
    while (iter->current_index < iter->proc_count) {
        pid_t pid = iter->pid_list[iter->current_index];
        /* Filter before reading: argv[0] lookup is the costly part. */
        if (process_matches_filter(pid, iter->filter) &&
            read_process_info(pid, proc, iter->filter->read_cmd) == 0) {
            iter->current_index++;
            return 0;
        }
        iter->current_index++;
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
    if (iter == NULL) {
        return -1;
    }
    free(iter->pid_list);
    memset(iter, 0, sizeof(*iter));
    return 0;
}

#endif
#else
/* Keep the translation unit non-empty on other platforms. */
typedef int cpulimit_process_iterator_apple_placeholder;
#endif
