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

#define PROC_FINDER_MAX_CANDIDATES 16

/**
 * @brief Check if a process exists and can be controlled by cpulimit
 *
 * @param pid Process ID to search for
 * @return Positive PID if process exists and can be signaled
 *         (kill(pid,0)==0), negative -PID if it exists but permission is
 *         denied (EPERM/EACCES), 0 if it does not exist or the PID is invalid
 */
pid_t find_process_by_pid(pid_t pid) {
    /*
     * Reject non-positive PIDs and PID 1 (init) before any kill() call.
     * PID 1 must never be signalled on any platform -- a kill to it, even
     * the kill(pid,0) existence probe, is forbidden because suspending or
     * interfering with init would freeze or crash the system. This guard
     * therefore covers the probe too, so no code path reaches kill() with
     * pid 1; there is no container or OS exception.
     */
    if (pid <= 0 || pid == 1) {
        return 0;
    }
    /* kill(pid, 0): existence + permission probe without signalling */
    if (kill(pid, 0) == 0) {
        return pid;
    }
    /* Process exists but we lack permission to signal it. Some systems
     * report EACCES rather than EPERM for an inaccessible process, so accept
     * both: neither is "does not exist". */
    if (errno == EPERM || errno == EACCES) {
        return -pid;
    }
    return 0;
}

/**
 * @brief Find a running process by its executable name or path
 *
 * @param process_name Name or absolute path of the executable to search for
 * @return Positive PID if found and accessible, negative -PID if found but
 *         permission denied, 0 if not found or invalid name
 *
 * Compares against argv[0] (full path if absolute, else basename). With
 * several matches the topmost ancestor wins, and among unrelated ones the
 * smallest PID wins; a controllable match is preferred over an uncontrollable
 * one, and a vanished winner falls back to another live candidate.
 *
 * @note Iterates every process, so prefer find_process_by_pid() when the PID is
 *       known; returns 0 on allocation or iterator errors.
 */
pid_t find_process_by_name(const char *process_name) {
    int found = 0;
    pid_t pid = 0;
    pid_t probe, best_pid, best_probe;
    pid_t candidates[PROC_FINDER_MAX_CANDIDATES];
    /* unsigned avoids a -Wstrict-overflow warning on the i + 1 bound below. */
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
     * after it. Matching against an empty string would produce false
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

    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(proc);
        return 0;
    }

    while (get_next_process(&iter, proc) != -1) {
        const char *cmd_cmp_name =
            full_path_cmp ? proc->command : get_file_basename(proc->command);
        /*
         * Never select PID 1 (init) by name: -e init would otherwise
         * resolve to the system's init process and route it into the limit
         * set, from which no kill must ever be sent. Skipping it here keeps
         * the name resolver consistent with the PID-input rejection and the
         * process-set guard, on every platform.
         */
        if (proc->pid == 1) {
            continue;
        }
        if (strcmp(cmd_cmp_name, process_cmp_name) == 0) {
            /*
             * Select this PID if:
             * - No match found yet (!found), OR
             * - This process is a descendant of the previous match
             *   (is_child_of(pid, proc->pid)) -- keep the higher/older one,
             * - The two matches are unrelated and this PID is smaller, which
             *   makes the winner independent of scan order. The
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
             * another live candidate. The array only caps the
             * MEMORY of candidates: the primary selection above keeps
             * running over every process, so with more than
             * PROC_FINDER_MAX_CANDIDATES matches the winner is still
             * chosen correctly and only a fallback could miss the ideal
             * survivor. Deliberately not raised: it bounds one fixed
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
        /*
         * The scan itself completed and this diagnostic is all the operator
         * can act on, so the selection below continues exactly as it does
         * on the normal path. Returning a PID here would skip the
         * existence recheck, the controllability probe and
         * the -PID contract, and would hand the caller a positive
         * PID for a process cpulimit cannot control at all: a limit run
         * that cannot be enforced, an EPERM warning per member per cycle,
         * and a zero exit status.
         */
        fprintf(stderr, "Failed to close process iterator\n");
    }

    /*
     * Verify the selected process still exists. If it vanished between the
     * scan and this recheck, fall back to another live candidate rather than
     * reporting a spurious "not found" that would throttle nothing.
     *
     * The fallback ranks the survivors by three tiers, highest first:
     * 1. controllable (probe > 0) beats uncontrollable (probe < 0);
     * 2. within one tier, an ancestor beats a descendant;
     * 3. otherwise the smaller PID wins.
     *
     * Reusing the scan's rule keeps the choice independent of the platform's
     * iteration order, and the controllability tier stops an uncontrollable
     * match from winning on PID alone and giving up a run that could have
     * limited something. The probe's sign still reaches the caller: -PID is
     * what makes it emit one "No permission to control process N", and it is
     * returned only after every survivor has been probed and none turned out
     * to be controllable.
     */
    if (n_candidates == 0) {
        return 0;
    }
    probe = find_process_by_pid(pid);
    if (probe > 0) {
        return probe;
    }
    best_pid = 0;
    best_probe = 0;
    if (probe < 0) {
        /*
         * The preferred match is alive but out of reach: remember it as a
         * second-choice result so a controllable candidate still beats it.
         */
        best_pid = pid;
        best_probe = probe;
    }
    for (i = 0; i < n_candidates; i++) {
        pid_t candidate = candidates[i];
        int better;
        if (candidate == pid) {
            continue;
        }
        probe = find_process_by_pid(candidate);
        if (probe == 0) {
            continue;
        }
        if (best_pid == 0) {
            best_pid = candidate;
            best_probe = probe;
            continue;
        }
        if ((probe > 0) != (best_probe > 0)) {
            better = probe > 0;
        } else {
            int candidate_is_ancestor = is_child_of(best_pid, candidate);
            int unrelated =
                !candidate_is_ancestor && !is_child_of(candidate, best_pid);
            better =
                candidate_is_ancestor || (unrelated && candidate < best_pid);
        }
        if (better) {
            best_pid = candidate;
            best_probe = probe;
        }
    }
    return best_probe;
}

/**
 * @brief Check whether a PID has since been taken over by another program
 *
 * @param pid Process ID to inspect
 * @param process_name Executable name or absolute path expected for that PID
 * @return 1 only when the PID is confirmed running a different executable, 0
 *         when it matches or no conclusion can be drawn
 *
 * Closes the window between resolving a name to a PID and starting to limit it;
 * the test is one-sided so any uncertainty keeps prior behavior unchanged.
 */
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
