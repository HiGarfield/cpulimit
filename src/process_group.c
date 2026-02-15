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

#include "process_group.h"

#include "list.h"
#include "process_iterator.h"
#include "process_table.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/**
 * @brief Find a process by its process ID and check accessibility
 * @param pid Process ID to search for
 * @return The PID if process exists and is controllable (kill(pid, 0) succeeds),
 *         -PID if process exists but lacks permission (EPERM),
 *         0 if process does not exist (ESRCH or other errors)
 * @note Uses kill(pid, 0) as a lightweight existence/permission check:
 *       - Returns immediately without sending actual signal
 *       - Checks both existence and permission in one system call
 *       - Avoids expensive /proc filesystem access
 */
pid_t find_process_by_pid(pid_t pid) {
    /* Reject invalid PIDs */
    if (pid <= 0) {
        return 0;
    }
    /* Check if process exists and can be signaled */
    if (kill(pid, 0) == 0) {
        return pid;
    }
    /* Process exists but permission denied (e.g., root-owned process) */
    if (errno == EPERM) {
        return -pid;
    }
    /* Process does not exist or other error occurred */
    return 0;
}

/**
 * @brief Find a process by its executable name or path
 * @param process_name Name or path of the executable to search for
 * @return Process ID if found and accessible,
 *         0 if not found,
 *         -PID if found but lacks permission (EPERM)
 * @note Matching behavior depends on process_name format:
 *       - Absolute path ("/path/to/prog"): compares full paths
 *       - Relative name ("prog"): compares basenames only
 *       If multiple matches exist, selects newest or child process:
 *       - Prefers descendants of previously matched PIDs
 *       - Uses is_child_of() to detect parent-child relationships
 *       Always loads command path from process iterator (expensive operation)
 */
pid_t find_process_by_name(const char *process_name) {
    /* PID of matching process (-1 = not found yet) */
    pid_t pid = -1;

    /* Process iterator and filter for enumerating all processes */
    struct process_iterator it;
    struct process_filter filter;
    struct process *proc;
    /*
     * Comparison mode flag:
     * - 1: Full path comparison (process_name starts with '/')
     * - 0: Basename-only comparison (process_name is relative)
     */
    int full_path_cmp;
    const char *process_cmp_name;

    if (process_name == NULL) {
        return 0;
    }

    /* Determine comparison mode based on leading '/' */
    full_path_cmp = process_name[0] == '/';
    /* Extract basename if doing basename comparison */
    process_cmp_name =
        full_path_cmp ? process_name : file_basename(process_name);
    /* Allocate temporary storage for process info during iteration */
    if ((proc = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process\n");
        exit(EXIT_FAILURE);
    }

    /* Configure iterator to scan all processes and read command paths */
    filter.pid = 0;              /* 0 = iterate all processes */
    filter.include_children = 0; /* No child filtering */
    filter.read_cmd = 1;         /* Must read command to compare names */
    if (init_process_iterator(&it, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        exit(EXIT_FAILURE);
    }

    /* Iterate through all processes searching for name match */
    while (get_next_process(&it, proc) != -1) {
        /* Extract comparison string from process command */
        const char *cmd_cmp_name =
            full_path_cmp ? proc->command : file_basename(proc->command);
        /* Check if name matches */
        if (strcmp(cmd_cmp_name, process_cmp_name) == 0) {
            /*
             * Multiple matches possible: select based on priority:
             * 1. First match (pid < 0)
             * 2. Child of previous match (maintains parent-child relationship)
             * This helps select the "main" process in a process family
             */
            if (pid < 0 || is_child_of(pid, proc->pid)) {
                pid = proc->pid;
            }
        }
    }
    free(proc);
    if (close_process_iterator(&it) != 0) {
        exit(EXIT_FAILURE);
    }

    /* Verify found process still exists and check permissions */
    return (pid > 0) ? find_process_by_pid(pid) : 0;
}

/**
 * @brief Initialize a process group structure for tracking a target process
 * @param pgroup Pointer to the process group structure to initialize
 * @param target_pid PID of the target (root) process
 * @param include_children If non-zero, track all descendant processes too
 * @return 0 on success (always succeeds or exits on error)
 * @note Allocates and initializes:
 *       - Hash table (size 2048) for O(1) PID lookups
 *       - Linked list for maintaining process group membership
 *       Performs initial population via update_process_group()
 *       Records timestamp for CPU usage calculation baseline
 */
int init_process_group(struct process_group *pgroup, pid_t target_pid,
                       int include_children) {
    /* Allocate hash table for efficient PID-based lookups */
    if ((pgroup->proctable = (struct process_table *)malloc(
             sizeof(struct process_table))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
    /* Initialize hash table with 2048 buckets */
    process_table_init(pgroup->proctable, 2048);
    pgroup->target_pid = target_pid;
    pgroup->include_children = include_children;

    /* Allocate linked list for maintaining group membership order */
    if ((pgroup->proclist = (struct list *)malloc(sizeof(struct list))) ==
        NULL) {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        exit(EXIT_FAILURE);
    }
    init_list(pgroup->proclist);

    /* Record baseline time for CPU usage calculations */
    if (get_current_time(&pgroup->last_update) != 0) {
        exit(EXIT_FAILURE);
    }
    /* Populate group with current processes */
    update_process_group(pgroup);
    return 0;
}

/**
 * @brief Clean up and free all resources associated with a process group
 * @param pgroup Pointer to the process group structure to close
 * @return 0 on success (always succeeds)
 * @note Frees:
 *       - All process list nodes (but NOT process data - owned by hash table)
 *       - All hash table buckets and process data
 *       - Process list and hash table structures themselves
 *       Safe to call multiple times (NULL-checks all pointers)
 */
int close_process_group(struct process_group *pgroup) {
    if (pgroup->proclist != NULL) {
        /* Clear list nodes without freeing data (data owned by proctable) */
        clear_list(pgroup->proclist);
        free(pgroup->proclist);
        pgroup->proclist = NULL;
    }

    if (pgroup->proctable != NULL) {
        /* Free all process data and hash table structure */
        process_table_destroy(pgroup->proctable);
        free(pgroup->proctable);
        pgroup->proctable = NULL;
    }

    return 0;
}

/**
 * @brief Duplicate a process structure by deep copy
 * @param proc Pointer to the source process structure to duplicate
 * @return Pointer to newly allocated process copy (never returns NULL)
 * @note Exits on allocation failure (no error return)
 *       Performs full memcpy of entire structure including:
 *       - PID, PPID
 *       - CPU time and usage statistics
 *       - Command path string
 *       Caller must free() returned pointer when done
 */
static struct process *process_dup(const struct process *proc) {
    struct process *p;
    if ((p = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        exit(EXIT_FAILURE);
    }
    return (struct process *)memcpy(p, proc, sizeof(struct process));
}

/*
 * Smoothing factor for exponential moving average of CPU usage
 * Formula: new_cpu = (1-ALPHA)*old_cpu + ALPHA*sample
 * ALPHA=0.08 means: 92% old value, 8% new sample
 * - Balances responsiveness vs. noise filtering
 * - Time constant ≈ 12.5 samples for 63% convergence
 */
#define ALPHA 0.08
/*
 * Minimum time delta (milliseconds) between CPU usage measurements
 * Prevents noisy/invalid readings from short time intervals
 * 20ms chosen as reasonable minimum for stable measurements
 */
#define MIN_DT 20

/**
 * @brief Update the process group with current process information
 * @param pgroup Pointer to the process group structure to update
 * @note This function refreshes the process list, calculates CPU usage,
 *       and removes terminated processes from the group.
 */
void update_process_group(struct process_group *pgroup) {
    struct process_iterator it;
    struct process *tmp_process;
    struct process_filter filter;
    struct timespec now;
    double dt;

    /* Get current time for delta calculation */
    if (get_current_time(&now) != 0) {
        exit(EXIT_FAILURE);
    }
    if ((tmp_process = (struct process *)malloc(sizeof(struct process))) ==
        NULL) {
        fprintf(stderr, "Memory allocation failed for tmp_process\n");
        exit(EXIT_FAILURE);
    }
    /* Time elapsed from previous sample (in ms) */
    dt = timediff_in_ms(&now, &pgroup->last_update);
    filter.pid = pgroup->target_pid;
    filter.include_children = pgroup->include_children;
    filter.read_cmd = 0;
    if (init_process_iterator(&it, &filter) != 0) {
        fprintf(stderr, "Failed to initialize process iterator\n");
        exit(EXIT_FAILURE);
    }

    /* Clear the process list for rebuilding */
    clear_list(pgroup->proclist);

    /* Iterate through processes and update the process group */
    while (get_next_process(&it, tmp_process) != -1) {
        struct process *p =
            process_table_find(pgroup->proctable, tmp_process->pid);
        if (p == NULL) {
            /* Process is new. Add it to the table and list */
            p = process_dup(tmp_process);
            /* Mark CPU usage as unknown for new processes */
            p->cpu_usage = -1;
            process_table_add(pgroup->proctable, p);
            if (add_elem(pgroup->proclist, p) == NULL) {
                fprintf(stderr,
                        "Failed to add process with PID %d to the list\n",
                        p->pid);
                exit(EXIT_FAILURE);
            }
        } else {
            double sample;
            if (add_elem(pgroup->proclist, p) == NULL) {
                fprintf(stderr,
                        "Failed to add process with PID %d to the list\n",
                        p->pid);
                exit(EXIT_FAILURE);
            }
            if (tmp_process->cputime < p->cputime) {
                /* PID reused, reset history */
                memcpy(p, tmp_process, sizeof(struct process));
                /* Mark CPU usage as unknown for reused PIDs */
                p->cpu_usage = -1;
                continue;
            }
            if (dt < 0) {
                /* Time went backwards, reset history */
                p->cputime = tmp_process->cputime;
                p->cpu_usage = -1;
                continue;
            }
            if (dt < MIN_DT) {
                continue;
            }
            /* Process exists. Update CPU usage */
            sample = (tmp_process->cputime - p->cputime) / dt;
            sample = MIN(sample, 1.0 * get_ncpu());
            if (p->cpu_usage < 0) {
                /* First time initialization */
                p->cpu_usage = sample;
            } else {
                /* CPU usage adjustment with exponential smoothing */
                p->cpu_usage = (1.0 - ALPHA) * p->cpu_usage + ALPHA * sample;
            }
            p->cputime = tmp_process->cputime;
        }
    }
    free(tmp_process);
    close_process_iterator(&it);

    /* Update last update time if enough time has passed for CPU usage
     * calculation or if time went backwards */
    if (dt < 0 || dt >= MIN_DT) {
        pgroup->last_update = now;
    }
}

/**
 * @brief Calculate the total CPU usage of the process group
 * @param pgroup Pointer to the process group structure
 * @return Total CPU usage of all processes in the group, or -1 if unknown
 */
double get_process_group_cpu_usage(const struct process_group *pgroup) {
    const struct list_node *node;
    double cpu_usage = -1;
    for (node = first_node(pgroup->proclist); node != NULL; node = node->next) {
        const struct process *p = (struct process *)node->data;
        if (p->cpu_usage < 0) {
            continue;
        }
        if (cpu_usage < 0) {
            cpu_usage = 0;
        }
        cpu_usage += p->cpu_usage;
    }
    return cpu_usage;
}
