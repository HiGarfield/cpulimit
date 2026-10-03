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

/**
 * @brief Number of candidate slots the fallback list starts with
 *
 * Only a starting size: the list doubles on demand, so the number of
 * processes sharing one executable name is bounded by nothing and is not
 * capped here.
 */
#define PROC_FINDER_INITIAL_CANDIDATES 8

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
    /*
     * Every match is kept so a vanished winner can fall back to another live
     * candidate. The list lives on the heap and doubles on demand, because
     * nothing bounds how many processes share one executable name (20 python3
     * interpreters, a pool of node workers, a batch of shell jobs) and a
     * fixed ceiling would drop the tail of a large group without a word.
     */
    pid_t *candidates = NULL;
    size_t n_candidates = 0;
    size_t candidates_cap = 0;
    size_t i;
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
         * the name resolver consistent with the PID-input rejection in cli.c
         * and the process-set guard, on every platform.
         *
         * This branch is silent on purpose: the operator-visible "this name
         * is PID 1" refusal is emitted once at argument-checking time
         * (cli.c, before the limiter starts), not on every watch-loop scan.
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
             * another live candidate. A dropped candidate is one the fallback
             * can no longer reach, and reaching exactly those is the only job
             * this list has, so the list grows instead of capping: the primary
             * selection above would still pick the right winner with a fixed
             * ceiling in place, which is what made the cap invisible.
             */
            if (n_candidates == candidates_cap) {
                size_t size_max = 0;
                size_t new_cap;
                pid_t *grown;
                /*
                 * Largest value size_t can hold, obtained by unsigned
                 * wraparound: 0 decremented once is reduced modulo one greater
                 * than the largest representable value (C89 6.1.2.1), which
                 * yields 2^N - 1 in every integer representation. Deliberately
                 * not spelled (size_t)-1: C89 6.3.1.3 calls an out-of-range
                 * signed-to-unsigned conversion implementation-defined, while
                 * the wraparound form never performs one.
                 */
                size_max--;
                new_cap = candidates_cap != 0 ? candidates_cap * 2
                                              : PROC_FINDER_INITIAL_CANDIDATES;
                /*
                 * Two ways to run out: the doubling wraps around, which a
                 * capacity below the current one can only mean, or the byte
                 * count would not fit into a size_t.
                 */
                if (new_cap < candidates_cap ||
                    new_cap > size_max / sizeof(*candidates)) {
                    fprintf(stderr, "Too many processes named '%s'\n",
                            process_cmp_name);
                    free(candidates);
                    free(proc);
                    close_process_iterator(&iter);
                    return 0;
                }
                grown = (pid_t *)realloc(candidates, new_cap * sizeof(*grown));
                if (grown == NULL) {
                    fprintf(stderr,
                            "Memory allocation failed for the candidates\n");
                    free(candidates);
                    free(proc);
                    close_process_iterator(&iter);
                    return 0;
                }
                candidates = grown;
                candidates_cap = new_cap;
            }
            candidates[n_candidates++] = proc->pid;
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
        free(candidates);
        fprintf(stderr, "Process '%s' cannot be found\n", process_cmp_name);
        return 0;
    }
    probe = find_process_by_pid(pid);
    if (probe > 0) {
        free(candidates);
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
    free(candidates);
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
