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

#include "path_util.h"
#include "process_iterator.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of name matches kept for fallback when the preferred one
 * vanishes between selection and the existence recheck (BUG-056). */
#define PROC_FINDER_MAX_CANDIDATES 16

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
#ifdef CPULIMIT_TEST_BUILD
    /* When the harness arms it, probe existence from the scripted iterator
     * seam instead of a real kill(pid, 0) so name-based lookup tests can drive
     * the final recheck deterministically (BUG-055/BUG-056). */
    if (seam_find_by_pid_override) {
        return cpulimit_test_find_by_pid(pid);
    }
#endif
    /*
     * Attempt to send null signal (doesn't actually signal, just checks
     * permission).
     */
    if (kill(pid, 0) == 0) {
        return pid;
    }
    /* Process exists but we lack permission to signal it.  Some systems
     * report EACCES rather than EPERM for an inaccessible process, so accept
     * both: neither is "does not exist". */
    if (errno == EPERM || errno == EACCES) {
        return -pid;
    }
    /* Process does not exist (errno is ESRCH or other error) */
    return 0;
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
 * When multiple matches exist, the topmost ancestor is preferred; among
 * unrelated matches the smallest PID wins, which makes the choice
 * independent of the platform's process iteration order.  This heuristic
 * helps ensure that if a parent process spawns children with the same name,
 * the parent is chosen.  If the chosen process vanishes before the
 * existence recheck, the best surviving candidate is selected by the same
 * rule.
 *
 * @note Returns 0 immediately for NULL or empty process_name
 * @note Iterates through all processes in the system, which may be slow on
 *       systems with many processes. For known PIDs, use find_process_by_pid().
 * @note On critical errors (e.g., memory allocation or iterator
 *       initialization failure) returns 0, letting the caller treat the
 *       target as "not found" and decide, instead of aborting the run.
 */
pid_t find_process_by_name(const char *process_name) {
    int found = 0;
    pid_t pid = 0;
    pid_t probe, fallback, fallback_probe;
    pid_t candidates[PROC_FINDER_MAX_CANDIDATES];
    /* unsigned: as signed counters the fallback loop below needs the
       assumption that i + 1 does not overflow to be folded, which
       -Wstrict-overflow reports on older compilers. */
    unsigned int n_candidates = 0;
    unsigned int i;
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
        full_path_cmp ? process_name : get_file_basename(process_name);
    /*
     * Reject an empty comparison name (e.g. process_name == "bin/").
     * get_file_basename("bin/") returns "" because the last '/' has nothing
     * after it.  Matching against an empty string would produce false
     * positives for any process whose argv[0] also ends with '/'.
     */
    if (process_cmp_name[0] == '\0') {
        return 0;
    }
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "Memory allocation failed for the process\n");
        return 0;
    }

    /* Configure iterator to scan all processes and read command names */
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(proc);
        return 0;
    }

    /* Scan all processes to find matching executable */
    while (get_next_process(&iter, proc) != -1) {
        const char *cmd_cmp_name =
            full_path_cmp ? proc->command : get_file_basename(proc->command);
        /* Check if this process matches the target name */
        if (strcmp(cmd_cmp_name, process_cmp_name) == 0) {
            /*
             * Select this PID if:
             * - No match found yet (!found), OR
             * - This process is a descendant of the previous match
             *   (is_child_of(pid, proc->pid)) -- keep the higher/older one,
             * - The two matches are unrelated and this PID is smaller, which
             *   makes the winner independent of scan order (BUG-055).  The
             *   /proc readdir / proc_listpids / kvm_getprocs order is not
             *   guaranteed, so without this tie-break the chosen target would
             *   change across runs.
             */
            int candidate_is_descendant = is_child_of(pid, proc->pid);
            int unrelated =
                !candidate_is_descendant && !is_child_of(proc->pid, pid);
            if (!found || candidate_is_descendant ||
                (unrelated && proc->pid < pid)) {
                pid = proc->pid;
                found = 1;
            }
            /*
             * Remember every match so a vanished winner can fall back to
             * another live candidate (BUG-056).  The array only caps the
             * MEMORY of candidates: the primary selection above keeps
             * running over every process, so with more than
             * PROC_FINDER_MAX_CANDIDATES matches the winner is still
             * chosen correctly and only a fallback could miss the ideal
             * survivor.  Deliberately not raised: it bounds one fixed
             * array on the stack, and 16 simultaneous name matches is
             * already far beyond realistic use.
             */
            if (n_candidates < PROC_FINDER_MAX_CANDIDATES) {
                candidates[n_candidates++] = proc->pid;
            }
        }
    }
    free(proc);
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
        /*
         * The scan itself completed, so degrade gracefully and report
         * whatever was found instead of aborting the whole run.
         */
        return found ? pid : 0;
    }

    /*
     * Verify the selected process still exists.  If it vanished between the
     * scan and this recheck, fall back to another live candidate instead of
     * giving up entirely: a still-running match is better than a spurious
     * "not found" that would make cpulimit throttle nothing (BUG-056).
     *
     * The fallback adjudicates the surviving candidates with the same rule
     * the scan above used -- an ancestor beats a descendant, and unrelated
     * candidates are decided by the smaller PID (N3).  Picking the first
     * survivor in candidates[] order would inherit the platform's process
     * iteration order, which the primary selection deliberately does not
     * depend on.
     *
     * The probe's sign must survive: it reports -PID for a process that
     * exists but cannot be controlled (EPERM/EACCES), and the caller turns
     * that into a single "No permission to control process N" instead of
     * limping through a limit run it cannot enforce (BUG-061).  Return the
     * probe result itself rather than using it as a boolean.
     */
    if (n_candidates == 0) {
        return 0;
    }
    probe = find_process_by_pid(pid);
    if (probe != 0) {
        return probe;
    }
    fallback = 0;
    fallback_probe = 0;
    for (i = 0; i < n_candidates; i++) {
        pid_t candidate = candidates[i];
        int candidate_is_ancestor, unrelated;
        if (candidate == pid) {
            continue;
        }
        probe = find_process_by_pid(candidate);
        if (probe == 0) {
            continue;
        }
        if (fallback == 0) {
            fallback = candidate;
            fallback_probe = probe;
            continue;
        }
        candidate_is_ancestor = is_child_of(fallback, candidate);
        unrelated = !candidate_is_ancestor && !is_child_of(candidate, fallback);
        if (candidate_is_ancestor || (unrelated && candidate < fallback)) {
            fallback = candidate;
            fallback_probe = probe;
        }
    }
    return fallback_probe;
}

int process_has_other_name(pid_t pid, const char *process_name) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    const char *process_cmp_name;
    int full_path_cmp;
    int other_name = 0;

    if (pid <= 0 || process_name == NULL || process_name[0] == '\0') {
        return 0;
    }
    full_path_cmp = process_name[0] == '/';
    process_cmp_name =
        full_path_cmp ? process_name : get_file_basename(process_name);
    if (process_cmp_name[0] == '\0') {
        return 0;
    }
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        return 0;
    }
    /*
     * A single-PID filter reads one /proc entry, which is all this check
     * costs; there is no need to walk the whole process table again.
     */
    filter.pid = pid;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        free(proc);
        return 0;
    }
    if (get_next_process(&iter, proc) == 0) {
        const char *cmd_cmp_name;
        cmd_cmp_name =
            full_path_cmp ? proc->command : get_file_basename(proc->command);
        if (strcmp(cmd_cmp_name, process_cmp_name) != 0) {
            other_name = 1;
        }
    }
    free(proc);
    close_process_iterator(&iter);
    return other_name;
}
