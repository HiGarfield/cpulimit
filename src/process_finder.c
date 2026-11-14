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
 * @def PROC_FINDER_INITIAL_CANDIDATES
 * @brief Initial candidate capacity; the list grows on demand.
 */
#define PROC_FINDER_INITIAL_CANDIDATES 8

/**
 * @brief Check whether a process exists and can be controlled.
 *
 * @param pid PID to probe.
 * @return pid when kill(pid, 0) succeeds, -pid on EPERM or EACCES, 0 when
 *         the PID is invalid, absent or PID 1.
 */
pid_t find_process_by_pid(pid_t pid) {
    /* Reject non-positive PIDs and PID 1 before any kill() call. */
    /* No signal, including the kill(pid, 0) probe, may reach init. */
    if (pid <= 0 || pid == 1) {
        return 0;
    }
    if (kill(pid, 0) == 0) {
        return pid;
    }
    /* EACCES and EPERM both mean the process exists but is out of reach. */
    if (errno == EPERM || errno == EACCES) {
        return -pid;
    }
    return 0;
}

/**
 * @brief Compare a target name against one process's argv[0].
 *
 * @param process_name Name or absolute path as typed by the operator.
 * @param command argv[0] as reported by the OS.
 * @return 1 when the two name the same executable, 0 otherwise.
 *
 * An absolute argument is compared in full; any other name by basename.
 * Shared by the finder, the PID-reuse check and cli.c's init-name check so
 * all three agree on what counts as a match.
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
    /* An empty comparison name would match every argv[0] ending in '/'. */
    if (cmp_name[0] == '\0') {
        return 0;
    }
    cmd_cmp_name = full_path_cmp ? command : get_file_basename(command);
    return strcmp(cmd_cmp_name, cmp_name) == 0;
}

/**
 * @brief Check whether any process other than PID 1 carries this name.
 *
 * @param process_name Name or absolute path as typed by the operator.
 * @return 1 when a non-init match exists, 0 when a clean, completed scan
 *         proves none does, -1 when the answer cannot be established.
 *
 * @note Only a completed scan closed cleanly may answer 0, because cli.c
 *       refuses the name on that value.
 */
int process_name_has_non_init_match(const char *process_name) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int found = 0;
    int scan_rc = 0;

    if (process_name == NULL || process_name[0] == '\0') {
        return -1;
    }
    proc = (struct process *)malloc(sizeof(*proc));
    if (proc == NULL) {
        return -1;
    }
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        free(proc);
        return -1;
    }
    while (found == 0 && (scan_rc = get_next_process(&iter, proc)) == 0) {
        if (proc->pid != 1 &&
            process_name_matches_cmd(process_name, proc->command)) {
            found = 1;
        }
    }
    free(proc);
    /* A failed close is not a completed scan. */
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
        return -1;
    }
    if (scan_rc == PROCESS_ITER_ERROR) {
        return -1;
    }
    return found;
}

/**
 * @brief Tell whether no other candidate is an ancestor of this one.
 *
 * @param candidates Every PID that matched the name.
 * @param count Number of candidates.
 * @param pid The candidate to test.
 * @return Non-zero when pid is the topmost of its family.
 */
static int candidate_is_topmost(const pid_t *candidates, size_t count,
                                pid_t pid) {
    unsigned depth = 0;
    pid_t current = pid;
    while (current > 1 && depth < IS_CHILD_MAX_DEPTH) {
        pid_t parent = getppid_of(current);
        size_t i;
        /* Retry a transient getppid_of() failure, as is_child_of_via() does. */
        if (parent < 0) {
            parent = getppid_of(current);
        }
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
 * @brief Pick the preferred match out of every process sharing a name.
 *
 * @param candidates Matching PIDs.
 * @param count Number of candidates.
 * @return The preferred PID, or 0 when count is 0.
 *
 * A topmost candidate is preferred over a descendant, then the smallest PID
 * wins. Both keys depend on one candidate alone, so the result does not
 * depend on the platform's scan order.
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
 * @brief Find a running process by executable name or path.
 *
 * @param process_name Name or absolute path of the executable.
 * @return Positive PID when found and accessible, negative -PID when
 *         permission is denied, 0 when not found or on a failed scan.
 *
 * Compares against argv[0], in full for an absolute argument and otherwise
 * by basename. Among matches the topmost ancestor wins, then the smallest
 * PID; a controllable match is preferred and a vanished winner falls back.
 *
 * @note Scans every process; prefer find_process_by_pid() when the PID is
 *       known. A failed close is reported but does not discard the result.
 */
pid_t find_process_by_name(const char *process_name) {
    pid_t pid = 0;
    pid_t probe, best_pid, best_probe;
    int best_topmost;
    /* Keep every match: the list grows so no candidate is silently dropped. */
    pid_t *candidates = NULL;
    size_t n_candidates = 0;
    size_t candidates_cap = 0;
    size_t i;
    int scan_rc = 0;
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int full_path_cmp;
    const char *process_cmp_name;

    if (process_name == NULL || process_name[0] == '\0') {
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

    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 1;
    if (init_process_iterator(&iter, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        free(proc);
        return 0;
    }

    while ((scan_rc = get_next_process(&iter, proc)) == 0) {
        /* Never select PID 1 by name; cli.c reports that once. */
        if (proc->pid == 1) {
            continue;
        }
        if (process_name_matches_cmd(process_name, proc->command)) {
            /* Grow the list: a dropped match cannot be a fallback. */
            if (n_candidates == candidates_cap) {
                size_t size_max = 0;
                size_t new_cap;
                pid_t *grown;
                /* Use unsigned wraparound for SIZE_MAX, not a signed cast. */
                size_max--;
                new_cap = candidates_cap != 0 ? candidates_cap * 2
                                              : PROC_FINDER_INITIAL_CANDIDATES;
                /* Doubling wrapped or the byte count overflows. */
                if (new_cap < candidates_cap ||
                    new_cap > size_max / sizeof(*candidates)) {
                    fprintf(stderr, "Too many processes named '%s'\n",
                            process_cmp_name);
                    free(candidates);
                    free(proc);
                    if (close_process_iterator(&iter) != 0) {
                        fprintf(stderr, "Failed to close process iterator\n");
                    }
                    return 0;
                }
                grown = (pid_t *)realloc(candidates, new_cap * sizeof(*grown));
                if (grown == NULL) {
                    fprintf(stderr,
                            "Memory allocation failed for the candidates\n");
                    free(candidates);
                    free(proc);
                    if (close_process_iterator(&iter) != 0) {
                        fprintf(stderr, "Failed to close process iterator\n");
                    }
                    return 0;
                }
                candidates = grown;
                candidates_cap = new_cap;
            }
            candidates[n_candidates++] = proc->pid;
        }
    }
    free(proc);
    /* A failed close is reported but does not discard a completed scan. */
    if (close_process_iterator(&iter) != 0) {
        fprintf(stderr, "Failed to close process iterator\n");
    }
    /* Discard a partial scan: candidates holds only a prefix of the table. */
    if (scan_rc == PROCESS_ITER_ERROR) {
        free(candidates);
        return 0;
    }

    /* Re-probe the winner; a vanished one falls back. */
    /* Rank survivors by controllability, then ancestry, then PID. */
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
        /* Use the same keys as the scan so the fallback agrees with it. */
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
 * @brief Check whether a PID now runs a different executable.
 *
 * @param pid PID to inspect.
 * @param process_name Expected executable name or path.
 * @return 1 only when a different executable is confirmed, 0 otherwise.
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
    /* A single-PID filter reads just one process entry. */
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
