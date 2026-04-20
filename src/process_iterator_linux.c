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

#ifndef CPULIMIT_PROCESS_ITERATOR_LINUX_C
#define CPULIMIT_PROCESS_ITERATOR_LINUX_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_iterator.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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
    if (iter == NULL || filter == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->filter = filter;
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        /*
         * Optimization: when querying a single process without children,
         * we can skip opening /proc directory entirely
         */
        iter->proc_dir = NULL;
        return 0;
    }
    /* Open /proc directory for iterating process entries */
    iter->proc_dir = opendir("/proc");
    if (iter->proc_dir == NULL) {
        perror("opendir");
        close_process_iterator(iter);
        return -1;
    }
    return 0;
}

/**
 * @brief Extract process information from Linux /proc filesystem
 * @param pid Process ID to query
 * @param proc Pointer to process structure to populate
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
static int read_process_info(pid_t pid, struct process *proc, int read_cmd) {
    char statfile[64], cmdline_path[64], state;
    char *buffer, *endptr;
    const char *p;
    double user_time, sys_time;
    long ppid;
    int skip_idx, cmdline_fd;
    ssize_t bytes_read;
    static long sc_clk_tck = -1;

    memset(proc, 0, sizeof(struct process));
    proc->pid = pid;

    /* Parse /proc/[pid]/stat for process state and timing information */
    sprintf(statfile, "/proc/%ld/stat", (long)pid);
    buffer = read_line_from_file(statfile);
    if (buffer == NULL) {
        return -1;
    }
    /*
     * Find the last ')' to handle process names containing parentheses.
     * Format: pid (comm) state ppid ... utime stime ...
     */
    p = strrchr(buffer, ')');
    if (p == NULL) {
        free(buffer);
        return -1;
    }
    /*
     * Parse stat fields manually for performance (replaces sscanf).
     * After ')': state ppid pgrp session tty_nr tpgid flags
     *            minflt cminflt majflt cmajflt utime stime ...
     */
    p++; /* Skip ')' */
    /* Skip whitespace before state (matches sscanf " " before %c) */
    while (isspace((unsigned char)*p)) {
        p++;
    }
    /* Read state character (equivalent to sscanf %c) */
    if (*p == '\0') {
        free(buffer);
        return -1;
    }
    state = *p;
    p++;
    /* Parse ppid - strtol skips leading whitespace (equivalent to %ld) */
    errno = 0;
    ppid = strtol(p, &endptr, 10);
    if (endptr == p || errno != 0) {
        free(buffer);
        return -1;
    }
    p = endptr;
    /* Skip 9 fields between ppid and utime (pgrp through cmajflt) */
    for (skip_idx = 0; skip_idx < 9; skip_idx++) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            free(buffer);
            return -1;
        }
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
    }
    /* Parse user_time - strtod skips leading whitespace (equiv. %lf) */
    errno = 0;
    user_time = strtod(p, &endptr);
    if (endptr == p || errno != 0) {
        free(buffer);
        return -1;
    }
    p = endptr;
    /* Parse sys_time - strtod skips leading whitespace (equiv. %lf) */
    errno = 0;
    sys_time = strtod(p, &endptr);
    if (endptr == p || errno != 0) {
        free(buffer);
        return -1;
    }
    /* Validate parsed fields */
    if (!isalpha((unsigned char)state) || strchr("ZXx", state) != NULL ||
        ppid <= 0 || user_time < 0 || sys_time < 0) {
        free(buffer);
        return -1;
    }
    free(buffer);
    proc->ppid = long2pid_t(ppid);
    if (proc->ppid < 0) {
        return -1;
    }
    /* Initialize clock ticks per second on first call */
    if (sc_clk_tck < 0) {
        errno = 0;
        sc_clk_tck = sysconf(_SC_CLK_TCK);
        if (sc_clk_tck <= 0) {
            perror("sysconf(_SC_CLK_TCK)");
            exit(EXIT_FAILURE);
        }
    }
    /* Convert CPU times from clock ticks to milliseconds */
    proc->user_time = user_time * 1000.0 / (double)sc_clk_tck;
    proc->sys_time = sys_time * 1000.0 / (double)sc_clk_tck;

    if (!read_cmd) {
        return 0;
    }
    /*
     * Read argv[0] from /proc/[pid]/cmdline using open/read to avoid
     * allocating a buffer for the entire argument list. The cmdline file
     * uses NUL bytes as argument separators (no newlines), so string
     * functions naturally stop at the first NUL, giving only argv[0].
     */
    sprintf(cmdline_path, "/proc/%ld/cmdline", (long)pid);
    cmdline_fd = open(cmdline_path, O_RDONLY);
    if (cmdline_fd < 0) {
        return -1;
    }
    do {
        bytes_read =
            read(cmdline_fd, proc->command, sizeof(proc->command) - 1);
    } while (bytes_read < 0 && errno == EINTR);
    if (close(cmdline_fd) != 0) {
        perror("close");
        /*
         * The file descriptor is closed regardless; the data read is
         * still valid.
         */
    }
    if (bytes_read <= 0) {
        return -1;
    }
    proc->command[bytes_read] = '\0';
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
    char *buffer, *endptr;
    const char *p;
    long ppid;

    /* Parse /proc/[pid]/stat for parent process ID */
    sprintf(statfile, "/proc/%ld/stat", (long)pid);
    buffer = read_line_from_file(statfile);
    if (buffer == NULL) {
        return (pid_t)-1;
    }
    /* Find last ')' to handle process names with parentheses */
    p = strrchr(buffer, ')');
    if (p == NULL) {
        free(buffer);
        return (pid_t)-1;
    }
    /*
     * Parse state and ppid manually for performance (replaces sscanf).
     * Format after ')': " state ppid ..."
     */
    p++; /* Skip ')' */
    /* Skip whitespace before state (matches sscanf " " before %c) */
    while (isspace((unsigned char)*p)) {
        p++;
    }
    /* Read state character (equivalent to sscanf %c) */
    if (*p == '\0') {
        free(buffer);
        return (pid_t)-1;
    }
    state = *p;
    p++;
    /* Parse ppid - strtol skips leading whitespace (equivalent to %ld) */
    errno = 0;
    ppid = strtol(p, &endptr, 10);
    if (endptr == p || errno != 0) {
        free(buffer);
        return (pid_t)-1;
    }
    /* Validate state and ppid, reject zombies and invalid PPIDs */
    if (!isalpha((unsigned char)state) || strchr("ZXx", state) != NULL ||
        ppid <= 0) {
        free(buffer);
        return (pid_t)-1;
    }
    free(buffer);
    return long2pid_t(ppid);
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
 * @brief Retrieve the next process matching the filter criteria
 * @param iter Pointer to the process_iterator structure
 * @param proc Pointer to process structure to populate with process information
 * @return 0 on success with process data in proc, -1 if no more processes or
 *         if iter, proc, or iter->filter is NULL
 *
 * Advances the iterator to the next process that satisfies the filter
 * criteria. The process structure is populated with information based on
 * the filter's read_cmd flag:
 * - Always populated: pid, ppid, user_time, sys_time
 * - Conditionally populated: command (only if filter->read_cmd is set)
 *
 * This function skips zombie processes, system processes (on FreeBSD/macOS),
 * and processes not matching the PID filter criteria.
 */
int get_next_process(struct process_iterator *iter, struct process *proc) {
    const struct dirent *dir_entry = NULL;

    if (iter == NULL || proc == NULL || iter->filter == NULL) {
        return -1;
    }
    if (iter->end_of_processes) {
        return -1;
    }

    /* Handle single process without children */
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        int ret =
            read_process_info(iter->filter->pid, proc, iter->filter->read_cmd);
        iter->end_of_processes = 1;
        return ret == 0 ? 0 : -1;
    }

    /* Guard against an uninitialized or failed iterator */
    if (iter->proc_dir == NULL) {
        iter->end_of_processes = 1;
        return -1;
    }

    /* Iterate through /proc entries to find matching processes */
    while (1) {
        pid_t pid;
        char *endptr;
        long long_pid;
        dir_entry = readdir(iter->proc_dir);
        if (dir_entry == NULL) {
            break;
        }
#ifdef _DIRENT_HAVE_D_TYPE
        /*
         * Optimization: skip non-directories if d_type is available.
         * DT_UNKNOWN must be checked because not all filesystems support
         * d_type.
         */
        if (dir_entry->d_type != DT_DIR && dir_entry->d_type != DT_UNKNOWN) {
            continue;
        }
#endif
        /* Process directories have numeric names */
        errno = 0;
        long_pid = strtol(dir_entry->d_name, &endptr, 10);
        if (errno != 0 || endptr == dir_entry->d_name || *endptr != '\0') {
            continue;
        }
        pid = long2pid_t(long_pid);
        if (pid <= 0) {
            continue;
        }
        /* Apply PID filter: match target PID or its descendants */
        if (iter->filter->pid != 0 && iter->filter->pid != pid &&
            !is_child_of(pid, iter->filter->pid)) {
            continue;
        }
        /* Read process info and skip on failure (e.g., process exited) */
        if (read_process_info(pid, proc, iter->filter->read_cmd) != 0) {
            continue;
        }
        return 0;
    }
    /* Reached end of /proc directory */
    iter->end_of_processes = 1;
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

    if (iter->proc_dir != NULL) {
        ret = closedir(iter->proc_dir);
        if (ret != 0) {
            perror("closedir");
        }
    }
    memset(iter, 0, sizeof(*iter));
    return ret;
}

#endif /* CPULIMIT_PROCESS_ITERATOR_LINUX_C */
#else
/* Placeholder to avoid empty compilation unit on non-Linux platforms. */
typedef int cpulimit_process_iterator_linux_placeholder;
#endif /* __linux__ */
