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

#ifdef __linux__

#ifndef __PROCESS_ITERATOR_LINUX_C
#define __PROCESS_ITERATOR_LINUX_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_iterator.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Initialize a process iterator with specified filter criteria
 * @param it Pointer to the process_iterator structure to initialize
 * @param filter Pointer to filter criteria, must remain valid during iteration
 * @return 0 on success, -1 on failure (Linux/FreeBSD), exits on failure (macOS)
 *
 * This function prepares the iterator for process enumeration. The behavior
 * varies by platform:
 * - Linux: Opens /proc directory, may skip if filtering single process
 * - FreeBSD: Opens kvm descriptor, retrieves process snapshot if needed
 * - macOS: Retrieves process ID list snapshot, may skip if filtering single
 *  process
 *
 * The filter pointer is stored and must remain valid until
 * close_process_iterator() is called.
 */
int init_process_iterator(struct process_iterator *it,
                          const struct process_filter *filter) {
    it->filter = filter;
    it->end_of_processes = 0;
    if (it->filter->pid != 0 && !it->filter->include_children) {
        /*
         * Optimization: when querying a single process without children,
         * we can skip opening /proc directory entirely
         */
        it->dip = NULL;
        return 0;
    }
    /* Open /proc directory for iterating process entries */
    if ((it->dip = opendir("/proc")) == NULL) {
        perror("opendir");
        return -1;
    }
    return 0;
}

/**
 * @brief Extract process information from Linux /proc filesystem
 * @param pid Process ID to query
 * @param p Pointer to process structure to populate
 * @param read_cmd Whether to read command path (0=skip, 1=read)
 * @return 0 on success, -1 on failure or if process is invalid
 *
 * Reads /proc/[pid]/stat for basic process information including PPID,
 * state, and CPU times. Optionally reads /proc/[pid]/cmdline for the
 * command path.
 *
 * The function rejects:
 * - Zombie processes (state Z, X, or x)
 * - Processes with invalid PPID (<= 0)
 * - Processes with negative CPU times
 *
 * CPU times are converted from clock ticks to milliseconds using
 * sysconf(_SC_CLK_TCK).
 */
static int read_process_info(pid_t pid, struct process *p, int read_cmd) {
    char statfile[64], exefile[64], state;
    char *buffer;
    const char *ptr_start;
    double usertime, systime;
    long ppid;
    static long sc_clk_tck = -1;

    memset(p, 0, sizeof(struct process));
    p->pid = pid;

    /* Parse /proc/[pid]/stat for process state and timing information */
    sprintf(statfile, "/proc/%ld/stat", (long)p->pid);
    if ((buffer = read_line_from_file(statfile)) == NULL) {
        return -1;
    }
    /*
     * Find the last ')' to handle process names containing parentheses.
     * Format: pid (comm) state ppid ... utime stime ...
     */
    ptr_start = strrchr(buffer, ')');
    if (ptr_start == NULL) {
        free(buffer);
        return -1;
    }
    if (sscanf(ptr_start,
               ") %c %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %lf %lf", &state,
               &ppid, &usertime, &systime) != 4 ||
        !isalpha(state) || strchr("ZXx", state) != NULL || ppid <= 0 ||
        usertime < 0 || systime < 0) {
        free(buffer);
        return -1;
    }
    free(buffer);
    p->ppid = (pid_t)ppid;
    /* Initialize clock ticks per second on first call */
    if (sc_clk_tck < 0) {
        sc_clk_tck = sysconf(_SC_CLK_TCK);
        if (sc_clk_tck <= 0) {
            fprintf(stderr, "sysconf(_SC_CLK_TCK) failed\n");
            exit(EXIT_FAILURE);
        }
    }
    /* Convert CPU times from clock ticks to milliseconds */
    p->cputime = (usertime + systime) * 1000.0 / (double)sc_clk_tck;

    if (!read_cmd) {
        return 0;
    }
    /* Read command path from /proc/[pid]/cmdline */
    sprintf(exefile, "/proc/%ld/cmdline", (long)p->pid);
    if ((buffer = read_line_from_file(exefile)) == NULL) {
        return -1;
    }
    strncpy(p->command, buffer, sizeof(p->command) - 1);
    p->command[sizeof(p->command) - 1] = '\0';
    free(buffer);
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
    char statfile[64], state;
    char *buffer;
    const char *ptr_start;
    long ppid;

    /* Parse /proc/[pid]/stat for parent process ID */
    sprintf(statfile, "/proc/%ld/stat", (long)pid);
    if ((buffer = read_line_from_file(statfile)) == NULL) {
        return (pid_t)-1;
    }
    /* Find last ')' to handle process names with parentheses */
    ptr_start = strrchr(buffer, ')');
    if (ptr_start == NULL) {
        free(buffer);
        return (pid_t)-1;
    }
    /* Extract state and PPID, reject zombies and invalid PPIDs */
    if (sscanf(ptr_start, ") %c %ld", &state, &ppid) != 2 || !isalpha(state) ||
        strchr("ZXx", state) != NULL || ppid <= 0) {
        free(buffer);
        return (pid_t)-1;
    }
    free(buffer);
    return (pid_t)ppid;
}

/**
 * @brief Retrieve process start time from /proc filesystem
 * @param pid Process ID to query
 * @param start_time Pointer to timespec structure to populate
 * @return 0 on success, -1 on failure
 *
 * Uses stat() on /proc/[pid] directory to get the modification time,
 * which corresponds to the process start time. This is used to detect
 * PID reuse by comparing start times when checking parent-child
 * relationships.
 *
 * On systems supporting high-resolution timestamps (POSIX.1-2008),
 * uses st_mtim for nanosecond precision. Otherwise falls back to
 * st_mtime with second precision.
 */
static int get_start_time(pid_t pid, struct timespec *start_time) {
    struct stat procfs_stat;
    char procfs_path[64];
    int ret;
    if (start_time == NULL) {
        return -1;
    }
    sprintf(procfs_path, "/proc/%ld", (long)pid);
    if ((ret = stat(procfs_path, &procfs_stat)) == 0) {
#if (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L) ||                \
    (defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 700)
        /* Use high-resolution timestamp if available */
        *start_time = procfs_stat.st_mtim;
#else
        /* Fall back to second-precision timestamp */
        start_time->tv_sec = procfs_stat.st_mtime;
        start_time->tv_nsec = 0;
#endif
    }
    return ret;
}

/**
 * @brief Compare two timestamps to determine chronological order
 * @param t1 First timestamp
 * @param t2 Second timestamp
 * @return 1 if t1 occurred before t2, 0 otherwise
 *
 * Performs chronological comparison of two timespec structures.
 * Compares seconds first, then nanoseconds if seconds are equal.
 */
static int earlier_than(const struct timespec *t1, const struct timespec *t2) {
    return t1->tv_sec < t2->tv_sec ||
           (t1->tv_sec == t2->tv_sec && t1->tv_nsec < t2->tv_nsec);
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
 * - Returns 0 if child_pid <= 0, parent_pid <= 0, or child_pid == parent_pid
 * - Returns 1 if parent_pid == 1 (all processes descend from init)
 * - Linux: Uses process start times to handle PID reuse
 * - FreeBSD/macOS: Relies on current process hierarchy only
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    int ret_child, ret_parent;
    struct timespec child_start_time, parent_start_time;
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    if (parent_pid == 1) {
        return 1;
    }
    /*
     * Get parent's start time to detect PID reuse.
     * If a process in the parent chain started before the supposed parent,
     * that PID was reused and cannot be a true ancestor.
     */
    ret_parent = get_start_time(parent_pid, &parent_start_time);
    while (child_pid > 1) {
        if (ret_parent == 0) {
            ret_child = get_start_time(child_pid, &child_start_time);
            /* Child started before parent means PID reuse occurred */
            if (ret_child == 0 &&
                earlier_than(&child_start_time, &parent_start_time)) {
                return 0;
            }
        }
        child_pid = getppid_of(child_pid);
        if (child_pid < 0) {
            return 0;
        }
        if (child_pid == parent_pid) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Validate that a string contains only decimal digits
 * @param str String to validate
 * @return 1 if string is non-empty and contains only digits, 0 otherwise
 *
 * Used to identify process directories in /proc, which have numeric names
 * corresponding to process IDs. Returns 0 for NULL, empty strings, or
 * strings containing any non-digit characters.
 */
static int is_numeric(const char *str) {
    if (str == NULL || *str == '\0') {
        return 0;
    }
    for (; *str != '\0'; str++) {
        if (!isdigit(*str)) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Retrieve the next process matching the filter criteria
 * @param it Pointer to the process_iterator structure
 * @param p Pointer to process structure to populate with process information
 * @return 0 on success with process data in p, -1 if no more processes
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
int get_next_process(struct process_iterator *it, struct process *p) {
    const struct dirent *dit = NULL;

    if (it->end_of_processes) {
        return -1;
    }

    /* Fast path for single process without children */
    if (it->filter->pid != 0 && !it->filter->include_children) {
        int ret = read_process_info(it->filter->pid, p, it->filter->read_cmd);
        it->end_of_processes = 1;
        return ret == 0 ? 0 : -1;
    }

    /* Iterate through /proc entries to find matching processes */
    while ((dit = readdir(it->dip)) != NULL) {
        pid_t pid;
#ifdef _DIRENT_HAVE_D_TYPE
        /*
         * Optimization: skip non-directories if d_type is available.
         * DT_UNKNOWN must be checked because not all filesystems support
         * d_type.
         */
        if (dit->d_type != DT_DIR && dit->d_type != DT_UNKNOWN) {
            continue;
        }
#endif
        /* Process directories have numeric names */
        if (!is_numeric(dit->d_name) || (pid = (pid_t)atol(dit->d_name)) <= 0) {
            continue;
        }
        /* Apply PID filter: match target PID or its descendants */
        if (it->filter->pid != 0 && it->filter->pid != pid &&
            !is_child_of(pid, it->filter->pid)) {
            continue;
        }
        /* Read process info and skip on failure (e.g., process exited) */
        if (read_process_info(pid, p, it->filter->read_cmd) != 0) {
            continue;
        }
        return 0;
    }
    /* Reached end of /proc directory */
    it->end_of_processes = 1;
    return -1;
}

/**
 * @brief Close the process iterator and release allocated resources
 * @param it Pointer to the process_iterator structure to close
 * @return 0 on success, -1 on failure
 *
 * Releases platform-specific resources allocated during initialization:
 * - Linux: Closes /proc directory stream
 * - FreeBSD: Frees process array and closes kvm descriptor
 * - macOS: Frees process ID list
 *
 * After this call, the iterator must not be used until re-initialized.
 */
int close_process_iterator(struct process_iterator *it) {
    int ret = 0;
    if (it == NULL) {
        return -1;
    }

    if (it->dip != NULL) {
        if ((ret = closedir(it->dip)) != 0) {
            perror("closedir");
        }
        it->dip = NULL;
    }

    it->end_of_processes = 0;
    it->filter = NULL;

    return ret == 0 ? 0 : -1;
}

#endif
#endif
