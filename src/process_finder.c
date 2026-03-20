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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_finder.h"

#include "process_iterator.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/**
 * @brief Check if a process exists and can be controlled by cpulimit
 * @param pid Process ID to search for
 * @return Positive PID if process exists and can be signaled
 *         (kill(pid,0)==0), negative -PID if process exists but permission
 *         denied (errno==EPERM), 0 if process does not exist (errno==ESRCH or
 *         invalid PID)
 *
 * Uses kill(pid, 0) as a lightweight probe to test process existence and
 * signal permission without actually sending a signal. This is the standard
 * POSIX method for checking process liveness and accessibility.
 */
pid_t find_process_by_pid(pid_t pid) {
    /* Reject invalid PIDs (must be positive) */
    if (pid <= 0) {
        return 0;
    }
    /* Attempt to send null signal (doesn't actually signal, just checks
     * permission) */
    if (kill(pid, 0) == 0) {
        return pid;
    }
    /* Process exists but we lack permission to signal it */
    if (errno == EPERM) {
        return -pid;
    }
    /* Process does not exist (errno is ESRCH or other error) */
    return 0;
}

/**
 * @brief Check whether a process's command matches a target name.
 * @param proc           Process to test.
 * @param target_name    Pre-normalized comparison name (must not be NULL or
 *                       empty; see the full_path_cmp description below).
 * @param full_path_cmp  Non-zero: compare proc->command against target_name
 *                       verbatim (both are absolute paths).
 *                       Zero: compare only the basename of proc->command
 *                       against target_name.
 * @return 1 if the command matches, 0 otherwise.
 */
static int command_matches_name(const struct process *proc,
                                const char *target_name, int full_path_cmp) {
    const char *cmd_name =
        full_path_cmp ? proc->command : file_basename(proc->command);
    return strcmp(cmd_name, target_name) == 0;
}

/**
 * @brief Find a running process by its executable name or path
 * @param process_name Name or absolute path of the executable to search for
 * @return Positive PID if found and accessible, negative -PID if found but
 *         permission denied, 0 if not found or invalid name
 *
 * Behavior depends on whether process_name is an absolute path:
 * - If process_name starts with '/': compares full absolute paths
 * - Otherwise: compares only the basename (executable name without directory)
 *
 * When multiple matches exist, selects the first process found, or if one is
 * an ancestor of another, prefers the ancestor. This heuristic helps ensure
 * that if a parent process spawns children with the same name, the parent is
 * chosen.
 *
 * @note Returns 0 immediately for NULL or empty process_name
 * @note Iterates through all processes in the system, which may be slow on
 *       systems with many processes. For known PIDs, use find_process_by_pid().
 * @note Calls exit(EXIT_FAILURE) on critical errors (e.g., memory allocation
 *       failure or iterator initialization failure)
 */
pid_t find_process_by_name(const char *process_name) {
    int found = 0;
    pid_t pid = 0;
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int full_path_cmp;
    const char *process_cmp_name;

    if (process_name == NULL || process_name[0] == '\0') {
        return 0;
    }

    /*
     * Determine comparison mode:
     * - Absolute path (starts with '/'): compare full paths
     * - Relative path/name: compare basenames only
     */
    full_path_cmp = process_name[0] == '/';
    process_cmp_name =
        full_path_cmp ? process_name : file_basename(process_name);
    /*
     * Reject an empty comparison name (e.g. process_name == "bin/").
     * file_basename("bin/") returns "" because the last '/' has nothing
     * after it.  Matching against an empty string would produce false
     * positives for any process whose argv[0] also ends with '/'.
     */
    if (process_cmp_name[0] == '\0') {
        return 0;
    }
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "Memory allocation failed for the process\n");
        exit(EXIT_FAILURE);
    }

    /* Configure iterator to scan all processes and read command names */
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(proc);
        exit(EXIT_FAILURE);
    }

    /* Scan all processes to find matching executable */
    while (get_next_process(&iter, proc) != -1) {
        if (command_matches_name(proc, process_cmp_name, full_path_cmp)) {
            /*
             * Select this PID if:
             * - No match found yet (!found), OR
             * - This process is an ancestor of the previous match
             * This heuristic prefers older/parent processes over newer/child
             * ones
             */
            if (!found || is_child_of(pid, proc->pid)) {
                pid = proc->pid;
                found = 1;
            }
        }
    }
    free(proc);
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
        exit(EXIT_FAILURE);
    }

    /* Verify the found process still exists and is accessible */
    return found ? find_process_by_pid(pid) : 0;
}
