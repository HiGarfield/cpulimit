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

#ifndef CPULIMIT_PROCESS_ITERATOR_H
#define CPULIMIT_PROCESS_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(__linux__) && !defined(__FreeBSD__) && !defined(__APPLE__)
#error "Platform not supported"
#endif

#include <sys/types.h>
#include <time.h>
#if defined(__linux__)
#include <dirent.h>
#include <limits.h>
#elif defined(__FreeBSD__)
#include <kvm.h>
#include <sys/param.h>
#elif defined(__APPLE__)
#include <libproc.h>
#endif

/**
 * @def CMD_BUFF_SIZE
 * @brief Size of the buffer holding a process's argv[0].
 *
 * Linux uses PATH_MAX, FreeBSD MAXPATHLEN and macOS
 * PROC_PIDPATHINFO_MAXSIZE.
 */
#if defined(__linux__)
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define CMD_BUFF_SIZE PATH_MAX
#elif defined(__FreeBSD__)
#define CMD_BUFF_SIZE MAXPATHLEN
#elif defined(__APPLE__)
#define CMD_BUFF_SIZE PROC_PIDPATHINFO_MAXSIZE
#endif

/**
 * @def UNKNOWN_START_TIME
 * @brief Sentinel for a start time the platform could not read.
 *
 * @note Real start times are non-negative, so a single comparison detects
 *       it. Only the Linux backend may report it.
 */
#define UNKNOWN_START_TIME (-1.0)

/**
 * @struct process
 * @brief Snapshot of one process.
 */
struct process {
    /**
     * @brief Process ID.
     */
    pid_t pid;

    /**
     * @brief Parent process ID.
     */
    pid_t ppid;

    /**
     * @brief Start time in seconds, or UNKNOWN_START_TIME.
     *
     * Linux counts since boot, macOS and FreeBSD since the epoch; only
     * same-platform equality is meaningful.
     */
    double start_time;

    /**
     * @brief Total user and system CPU time in milliseconds.
     */
    double cpu_time;

    /**
     * @brief Clock reading at which cpu_time was recorded.
     *
     * A sample uses this member's interval, not the set's last_update, so a
     * member found mid-cycle keeps its own baseline. Snapshots carry {0,0}.
     */
    struct timespec cpu_time_ts;

    /**
     * @brief Estimated CPU usage in core equivalents; -1.0 means unmeasured.
     */
    double cpu_usage;

    /**
     * @brief argv[0] as reported by the OS, truncated to CMD_BUFF_SIZE.
     */
    char command[CMD_BUFF_SIZE];

    /**
     * @brief Gates for repeated SIGCONT and SIGSTOP failure warnings.
     */
    int cont_warned;
    int stop_warned;

    /**
     * @brief Gate for the severe "may remain stopped" SIGCONT warning.
     */
    int resume_warned;

    /**
     * @brief Non-zero while a SIGSTOP by this group is still outstanding.
     *
     * Meaningful only for proc_table records; snapshots carry 0.
     */
    int suspended_by_us;
};

/**
 * @struct process_filter
 * @brief Criteria for process iteration.
 */
struct process_filter {
    /**
     * @brief PID to select, or 0 to iterate every process.
     */
    pid_t pid;

    /**
     * @brief Non-zero to include descendants of the filter PID.
     */
    int include_children;

    /**
     * @brief Non-zero to read argv[0] into process::command.
     */
    int read_cmd;
};

/**
 * @struct process_iterator
 * @brief Platform-specific state for enumerating processes.
 *
 * Linux walks /proc entry by entry; FreeBSD uses a kvm(3) snapshot and
 * macOS a proc_listpids() PID list. Observable differences:
 * - close_process_iterator() can fail on Linux and FreeBSD, never on macOS;
 * - an empty table is a completed scan on Linux but an init failure on the
 *   snapshot platforms;
 * - macOS and FreeBSD skip system processes and may yield PID 1; Linux
 *   rejects a parent <= 0, so it never yields it. Callers must skip PID 1;
 * - start_time uses different epochs, so only same-platform equality is
 *   meaningful for PID-reuse detection.
 */
struct process_iterator {
#if defined(__linux__)
    /**
     * @brief /proc directory stream (Linux).
     */
    DIR *proc_dir;

    /**
     * @brief Non-zero once /proc iteration is complete (Linux).
     */
    int end_of_processes;
#elif defined(__FreeBSD__)
    /**
     * @brief kvm(3) descriptor (FreeBSD).
     */
    kvm_t *kvm_descriptor;

    /**
     * @brief Snapshot of kinfo_proc entries (FreeBSD).
     */
    struct kinfo_proc *kinfo_procs;

    /**
     * @brief Number of entries in the snapshot (FreeBSD).
     */
    int proc_count;

    /**
     * @brief Current snapshot index (FreeBSD).
     */
    int current_index;
#elif defined(__APPLE__)
    /**
     * @brief Current PID-list index (macOS).
     */
    int current_index;

    /**
     * @brief Number of PIDs in the list (macOS).
     */
    int proc_count;

    /**
     * @brief Snapshot of PIDs from proc_listpids() (macOS).
     */
    pid_t *pid_list;
#endif

    /**
     * @brief Filter criteria applied during iteration.
     */
    const struct process_filter *filter;
};

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
                          const struct process_filter *filter);

/**
 * @def PROCESS_ITER_ERROR
 * @brief get_next_process() code for a scan that stopped on an error.
 *
 * @note Distinct from the -1 that means the table was read to its end.
 */
#define PROCESS_ITER_ERROR (-2)

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
int get_next_process(struct process_iterator *iter, struct process *proc);

/**
 * @brief Release the iterator's platform-specific resources.
 *
 * @param iter Iterator to close.
 * @return 0 on success, -1 on a close failure; macOS has none to fail.
 *
 * @note Re-initialize the iterator before using it again.
 */
int close_process_iterator(struct process_iterator *iter);

/**
 * @def IS_CHILD_MAX_DEPTH
 * @brief Maximum parent-chain steps in is_child_of_via().
 *
 * @note Bounds the walk against cycles created by PID reuse.
 */
#define IS_CHILD_MAX_DEPTH 1024

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
int is_child_of(pid_t child_pid, pid_t parent_pid);

/**
 * @brief Reads the parent of one process.
 *
 * @param context Handle supplied by the caller.
 * @param pid PID whose parent is wanted.
 * @return The parent PID, or a value at or below 0 on failure.
 */
typedef pid_t (*process_get_parent_fn)(void *context, pid_t pid);

/**
 * @brief Walk a parent chain using a platform-specific lookup.
 *
 * @param get_parent Reads one process's parent.
 * @param context Handle passed back to get_parent.
 * @param child_pid PID to test.
 * @param parent_pid PID of the potential ancestor.
 * @return 1 when parent_pid is in the ancestry chain, 0 otherwise.
 *
 * @note Shared by every backend; see is_child_of().
 */
int is_child_of_via(process_get_parent_fn get_parent, void *context,
                    pid_t child_pid, pid_t parent_pid);

/**
 * @brief Read the parent PID of a process.
 *
 * @param pid PID to query.
 * @return The parent PID, or -1 when the process is gone, a zombie, or the
 *         platform lookup fails.
 *
 * @note getppid_of(1) differs: Linux returns -1, FreeBSD 0, macOS 0 or -1.
 */
pid_t getppid_of(pid_t pid);

/**
 * @brief Determine whether a PID satisfies the iterator filter.
 *
 * @param pid PID to evaluate.
 * @param filter Filter criteria.
 * @return 1 when the PID matches, 0 otherwise or when filter is NULL.
 *
 * A match is the filter PID itself, or a descendant when include_children
 * is set; a zero filter PID matches every process.
 */
int process_matches_filter(pid_t pid, const struct process_filter *filter);

/**
 * @brief Read the start time of one process.
 *
 * @param pid PID to query.
 * @return Start time in seconds, or UNKNOWN_START_TIME when unavailable.
 *
 * @note Used to confirm a PID still names the same process. A failed
 *       iterator close is reported but does not change the reading.
 */
double get_process_start_time(pid_t pid);

/**
 * @brief Report whether a process is currently stopped.
 *
 * @param pid PID to query.
 * @return 1 when stopped, 0 when running, gone or unreadable.
 *
 * @note Read-only: sends no signal and starts no scan.
 */
int process_is_stopped(pid_t pid);

#ifdef __cplusplus
}
#endif

#endif
