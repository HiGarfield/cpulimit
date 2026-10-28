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
 *         denied (EPERM/EACCES), 0 if it does not exist, the PID is invalid,
 *         or the PID is 1 (init), which is never signalled on any platform
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
 * @brief Compare a target name or path against one process's argv[0]
 *
 * @param process_name Name or absolute path as typed by the operator
 * @param command argv[0] of the process being compared, as the OS reports it
 * @return 1 when the two name the same executable under cpulimit's rules,
 *         0 when they do not or either string is unusable
 *
 * An argument starting with '/' is compared in full, anything else by basename
 * only, so "-e myapp" matches "/usr/bin/myapp" and "./dir/myapp" alike while
 * "-e /usr/bin/myapp" matches nothing else. One definition rather than one per
 * caller because three questions -- which match wins, whether a PID still runs
 * the target, and whether a name leads anywhere beyond init -- must agree on
 * what counts as a match; they answer it for the same operator argument, and a
 * second copy is free to drift.
 */
int process_name_matches_cmd(const char *process_name, const char *command) {
    const char *cmp_name;
    const char *cmd_cmp_name;
    int full_path_cmp;

    if (process_name == NULL || command == NULL || process_name[0] == '\0') {
        return 0;
    }
    full_path_cmp = process_name[0] == '/';
    cmp_name = full_path_cmp ? process_name : get_file_basename(process_name);
    /*
     * An empty comparison name is structurally unmatchable: matching against
     * "" would claim every process whose argv[0] also ends with '/'.
     */
    if (cmp_name[0] == '\0') {
        return 0;
    }
    cmd_cmp_name = full_path_cmp ? command : get_file_basename(command);
    return strcmp(cmd_cmp_name, cmp_name) == 0;
}

/**
 * @brief Check whether any process other than PID 1 carries this name
 *
 * @param process_name Name or absolute path as typed by the operator
 * @return 1 when at least one process other than PID 1 has a matching argv[0],
 *         0 when none does, or when the name is unusable or the scan fails
 *
 * Answers the narrower question behind the Linux-only refusal in cli.c. The
 * name lookup skips PID 1 on every platform, so a name init happens to wear
 * can still have ordinary, perfectly limitable processes behind it; only a
 * name with nothing else is the dead end worth refusing up front. Whether any
 * of them answers to a signal is not asked here -- that is find_process_by_
 * name()'s job, and it decides which match wins, not whether one exists.
 *
 * Read-only: signals nothing, builds no candidate list, prints nothing.
 *
 * @note Returns 0 when the scan itself fails. Being told "nothing else" then
 *       costs a name refused here that every platform would have reported as
 *       not found a moment later, which is the one outcome shared anyway.
 */
int process_name_has_non_init_match(const char *process_name) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int found = 0;

    if (process_name == NULL || process_name[0] == '\0') {
        return 0;
    }
    proc = (struct process *)malloc(sizeof(*proc));
    if (proc == NULL) {
        return 0;
    }
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        free(proc);
        return 0;
    }
    while (found == 0 && get_next_process(&iter, proc) != -1) {
        if (proc->pid != 1 &&
            process_name_matches_cmd(process_name, proc->command)) {
            found = 1;
        }
    }
    free(proc);
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }
    return found;
}

/**
 * @brief Whether no other candidate is an ancestor of this one
 *
 * @param candidates Every PID that matched the name
 * @param count Number of entries in candidates
 * @param pid The candidate to test, a member of candidates
 * @return Non-zero when no other candidate sits above pid in the process
 *         tree, so pid is the topmost of its family
 *
 * Walks the parent chain instead of testing every other candidate with
 * is_child_of(): one chain is a handful of reads, while a candidate per
 * test is a chain walk for each of them.
 */
static int candidate_is_topmost(const pid_t *candidates, size_t count,
                                pid_t pid) {
    unsigned depth = 0;
    pid_t current = pid;
    while (current > 1 && depth < IS_CHILD_MAX_DEPTH) {
        pid_t parent = getppid_of(current);
        size_t i;
        if (parent <= 0 || parent == current) {
            break;
        }
        for (i = 0; i < count; i++) {
            if (candidates[i] == parent && candidates[i] != pid) {
                return 0;
            }
        }
        current = parent;
        depth++;
    }
    return 1;
}

/**
 * @brief Pick one match out of every process sharing a name
 *
 * @param candidates Every PID that matched the name
 * @param count Number of entries in candidates
 * @return The preferred PID, or 0 when count is 0
 *
 * Two keys, both of them properties of the candidate alone: a candidate no
 * other candidate descends from beats one that does -- the launcher rather
 * than the worker it started -- and otherwise the smallest PID wins.
 *
 * Those keys have to be properties of one candidate. Preferring "is an
 * ancestor of the other" directly is not transitive: with a parent, its
 * child and an unrelated process sharing one name, each pair prefers a
 * different one, so folding that comparison over the scan picks whichever
 * the platform happened to enumerate last, and the target of a -e run
 * changes from one scan to the next.
 */
static pid_t select_preferred_candidate(const pid_t *candidates, size_t count) {
    size_t i;
    pid_t best = 0;
    int best_topmost = 0;
    for (i = 0; i < count; i++) {
        int topmost = candidate_is_topmost(candidates, count, candidates[i]);
        if (best == 0 || (topmost && !best_topmost) ||
            (topmost == best_topmost && candidates[i] < best)) {
            best = candidates[i];
            best_topmost = topmost;
        }
    }
    return best;
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
    pid_t pid = 0;
    pid_t probe, best_pid, best_probe;
    int best_topmost;
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
        if (process_name_matches_cmd(process_name, proc->command)) {
            /*
             * Remember every match so a vanished winner can fall back to
             * another live candidate, and so the winner can be chosen once
             * every match is known: which match is preferred depends on
             * which other processes share the name, and the scan order is
             * not something the choice may depend on. See
             * select_preferred_candidate().
             *
             * A dropped candidate is one the fallback
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
    /*
     * A failed close does not change the selection below, which runs exactly
     * as it does on the normal path: the scan itself completed, and its
     * result is what the existence recheck, the controllability probe and the
     * -PID contract are for. Aborting here instead would hand the caller a
     * PID it could not trust, or a positive PID for a process it cannot
     * control at all -- a limit run that cannot be enforced and a zero exit
     * status. The failure is still reported.
     */
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }

    /*
     * Verify the selected process still exists. If it vanished between the
     * scan and this recheck, fall back to another live candidate rather than
     * reporting a spurious "not found" that would throttle nothing.
     *
     * The fallback ranks the survivors by three tiers, highest first:
     * 1. controllable (probe > 0) beats uncontrollable (probe < 0);
     * 2. within one tier, a candidate no other candidate descends from
     *    beats one that does;
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
        return 0;
    }
    pid = select_preferred_candidate(candidates, n_candidates);
    probe = find_process_by_pid(pid);
    if (probe > 0) {
        free(candidates);
        return probe;
    }
    best_pid = 0;
    best_probe = 0;
    best_topmost = 0;
    if (probe < 0) {
        /*
         * The preferred match is alive but out of reach: remember it as a
         * second-choice result so a controllable candidate still beats it.
         */
        best_pid = pid;
        best_probe = probe;
        best_topmost = candidate_is_topmost(candidates, n_candidates, pid);
    }
    for (i = 0; i < n_candidates; i++) {
        pid_t candidate = candidates[i];
        int better, topmost;
        if (candidate == pid) {
            continue;
        }
        probe = find_process_by_pid(candidate);
        if (probe == 0) {
            continue;
        }
        topmost = candidate_is_topmost(candidates, n_candidates, candidate);
        if (best_pid == 0) {
            best_pid = candidate;
            best_probe = probe;
            best_topmost = topmost;
            continue;
        }
        /*
         * The same two keys the scan's own choice uses, in the same order,
         * so the fallback agrees with it instead of offering a process the
         * scan would have ranked lower.
         */
        if ((probe > 0) != (best_probe > 0)) {
            better = probe > 0;
        } else if (topmost != best_topmost) {
            better = topmost;
        } else {
            better = candidate < best_pid;
        }
        if (better) {
            best_pid = candidate;
            best_probe = probe;
            best_topmost = topmost;
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
        fprintf(stderr, "Memory allocation failed for the process\n");
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
    if (get_next_process(&iter, proc) == 0 &&
        !process_name_matches_cmd(process_name, proc->command)) {
        other_name = 1;
    }
    free(proc);
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }
    return other_name;
}
