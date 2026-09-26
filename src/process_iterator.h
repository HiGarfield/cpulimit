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
 * @brief Maximum size for command path buffer, platform-dependent
 *
 * On Linux: Uses PATH_MAX (4096 bytes default) to accommodate full paths
 *           via /proc filesystem
 * On FreeBSD: Uses MAXPATHLEN from system headers
 * On macOS: Uses PROC_PIDPATHINFO_MAXSIZE from libproc
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
 * @brief Value of struct process::start_time when the platform could not
 *        provide one
 *
 * Real start times are never negative, so a caller can tell "unknown" from
 * "started at some point" with a single comparison and skip the check.
 */
#define UNKNOWN_START_TIME (-1.0)

/**
 * @struct process
 * @brief Represents a snapshot of process information
 *
 * This structure contains essential information about a process,
 * including its identity, resource usage, and executable path.
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
     * @brief Time at which this process started, in seconds.
     *
     * Reference point is platform-specific (seconds since boot on Linux,
     * since the epoch on macOS/FreeBSD); only same-platform equality is
     * meaningful, which is all PID-reuse detection needs. -1.0 means unknown
     * and must not be compared.
     */
    double start_time;

    /**
     * @brief Cumulative CPU time consumed by the process in milliseconds.
     * Includes both user and system time.
     */
    double cpu_time;

    /**
     * @brief Baseline moment cpu_time was recorded, same clock as a set's
     *        last_update.
     *
     * A sample divides a cpu_time delta by the interval from THIS timestamp,
     * not last_update, so a member discovered mid-cycle is not under-measured.
     * Meaningful only for proc_table records; iterator snapshots carry {0,0}.
     */
    struct timespec cpu_time_ts;

    /**
     * @brief Estimated CPU usage as a multiple of one core.
     * 0.0 = idle, N = N cores busy; -1.0 means not yet measured.
     */
    double cpu_usage;

    /**
     * @brief Absolute path to the process executable or command.
     * Size is platform-dependent (see CMD_BUFF_SIZE).
     */
    char command[CMD_BUFF_SIZE];

    /**
     * @brief Per-member throttles for repeated signal-failure warnings.
     *
     * A member whose SIGSTOP/SIGCONT cannot be delivered is retried every
     * cycle, so flagging the failure once per episode (cleared by a later
     * success) avoids flooding the terminal. Per-member rather than global so
     * concurrent members report independently. cont_warned gates the benign
     * "never suspended" SIGCONT failure.
     */
    int cont_warned;
    int stop_warned;

    /**
     * @brief Throttle for the severe "may remain stopped" SIGCONT failure.
     *
     * Separate from cont_warned: the severe episode depends on
     * suspended_by_us and can co-occur with the benign one, so it needs its
     * own flag (cleared by a successful SIGCONT).
     */
    int resume_warned;

    /**
     * @brief Non-zero while a successful SIGSTOP by this group is still
     *        outstanding (not yet undone by SIGCONT).
     *
     * A failed shutdown SIGCONT reports "left suspended" only for members this
     * flag marks; a member whose signals were never deliverable has been
     * running and must not be reported as stranded. Meaningful only for
     * proc_table records (snapshots carry 0).
     */
    int suspended_by_us;
};

/**
 * @struct process_filter
 * @brief Defines criteria for filtering processes during iteration
 *
 * This structure controls which processes are returned by the iterator
 * and what information is retrieved for each process.
 */
struct process_filter {
    /**
     * @brief Target process ID to filter by, or 0 to iterate all processes.
     * When non-zero, only this process (and optionally its descendants)
     * will be returned.
     */
    pid_t pid;

    /**
     * @brief Whether to include child processes of the target PID.
     * Only meaningful when pid is non-zero.
     * 0: Return only the specified process
     * 1: Return the process and all its descendants
     */
    int include_children;

    /**
     * @brief Whether to read the command path for each process.
     * 0: Skip reading command path (faster, process.command is empty)
     * 1: Read full executable path (slower, populates process.command)
     */
    int read_cmd;
};

/**
 * @struct process_iterator
 * @brief Platform-specific iterator for enumerating processes
 *
 * This structure maintains the state needed to iterate over system
 * processes. The internal members vary by platform to leverage
 * platform-specific APIs efficiently.
 *
 * Platform implementations:
 * - Linux: Reads /proc filesystem entries sequentially
 * - FreeBSD: Uses kvm(3) interface with snapshot approach
 * - macOS: Uses libproc with snapshot approach
 */
struct process_iterator {
#if defined(__linux__)
    /**
     * @brief Directory stream for /proc filesystem.
     * Each entry corresponds to a process directory (e.g., /proc/1234).
     */
    DIR *proc_dir;

    /**
     * @brief Flag indicating iteration is complete.
     * Set to 1 when readdir() returns NULL or single-process mode completes.
     */
    int end_of_processes;
#elif defined(__FreeBSD__)
    /**
     * @brief Kernel virtual memory descriptor for accessing process
     * information. Opened via kvm_openfiles() and used for all process queries.
     */
    kvm_t *kvm_descriptor;

    /**
     * @brief Snapshot of all process information structures.
     * Populated by kvm_getprocs() at initialization.
     */
    struct kinfo_proc *kinfo_procs;

    /**
     * @brief Total number of processes in the snapshot.
     */
    int proc_count;

    /**
     * @brief Current iteration index into the kinfo_procs array.
     */
    int current_index;
#elif defined(__APPLE__)
    /**
     * @brief Current iteration index into the pid_list array.
     */
    int current_index;

    /**
     * @brief Total number of process IDs in the list.
     */
    int proc_count;

    /**
     * @brief Snapshot of all process IDs in the system.
     * Populated by proc_listpids() at initialization.
     */
    pid_t *pid_list;
#endif

    /**
     * @brief Filter criteria to apply during iteration.
     * Determines which processes to return and what information to retrieve.
     */
    const struct process_filter *filter;
};

/**
 * @brief Initialize a process iterator with the given filter
 *
 * @param iter Pointer to the process_iterator structure to initialize
 * @param filter Pointer to filter criteria, must remain valid during iteration
 * @return 0 on success, -1 on failure (including NULL iter/filter or OOM);
 *         this function does not call exit()
 *
 * @note The filter pointer is stored and must remain valid until
 *       close_process_iterator() is called.
 */
int init_process_iterator(struct process_iterator *iter,
                          const struct process_filter *filter);

/**
 * @brief Retrieve the next process matching the filter criteria
 *
 * @param iter Pointer to the process_iterator structure
 * @param proc Pointer to process structure to populate with process information
 * @return 0 on success with process data in proc, -1 if no more processes or
 *         if iter, proc, or iter->filter is NULL
 *
 * Advances the iterator to the next process that satisfies the filter
 * criteria. The process structure is populated with information based on
 * the filter's read_cmd flag:
 * - Always populated: pid, ppid, cpu_time
 * - Conditionally populated: command (only if filter->read_cmd is set)
 *
 * This function skips zombie processes, system processes (on FreeBSD/macOS),
 * and processes not matching the PID filter criteria.
 */
int get_next_process(struct process_iterator *iter, struct process *proc);

/**
 * @brief Close the process iterator and release allocated resources
 *
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
int close_process_iterator(struct process_iterator *iter);

/**
 * @def IS_CHILD_MAX_DEPTH
 * @brief Maximum parent-chain traversal steps in is_child_of()
 *
 * Bounds the while loop that walks parent PIDs to prevent an infinite loop
 * when PID reuse creates a cycle of length >= 2 in the parent chain (e.g.,
 * process A has ppid B and process B has ppid A). No legitimate process tree
 * exceeds a few hundred levels, so 1024 provides ample headroom while
 * guaranteeing termination in all adversarial cases.
 */
#define IS_CHILD_MAX_DEPTH 1024

/**
 * @brief Determine if one process is a descendant of another
 *
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
 * - Returns 0 if the parent chain exceeds IS_CHILD_MAX_DEPTH steps (guards
 *   against infinite loops caused by PID reuse cycles)
 */
int is_child_of(pid_t child_pid, pid_t parent_pid);

/**
 * @brief Retrieve the parent process ID for a given process
 *
 * @param pid Process ID to query
 * @return Parent process ID on success, -1 on error
 *
 * @note Returns -1 if the process does not exist, is a zombie, or the lookup
 *       fails (per-platform backend: /proc, kvm, or libproc).
 */
pid_t getppid_of(pid_t pid);

#ifdef CPULIMIT_TEST_BUILD
/**
 * @brief Test seam backing getppid_of() inside is_child_of()
 *
 * @param pid Process whose parent PID is wanted
 * @return The fabricated parent PID, or the real one when the seam is unarmed
 *
 * is_child_of() routes its parent-PID lookups through this function when
 * seam_getppid_fabricate is set, so ancestor-chain breakage can be reproduced
 * deterministically. Compiled only into the test build.
 */
pid_t cpulimit_test_getppid_of(pid_t pid);

/**
 * @brief Non-zero: is_child_of() should use the getppid_of() seam. */
extern int seam_getppid_fabricate;

/**
 * @brief Test seam backing find_process_by_pid()'s existence probe
 *
 * @param pid Process whose liveness is being checked
 * @return The PID while the seam reports it alive, 0 once it is gone
 *
 * find_process_by_pid() routes its liveness check through this function when
 * seam_find_by_pid_override is set, instead of sending a real kill(pid, 0).
 * The seam reads the currently selected iterator frame, so a candidate the
 * scripted snapshot dropped reports as gone, which lets name-based lookup
 * tests drive the final recheck deterministically. Compiled only into the
 * test build.
 */
pid_t cpulimit_test_find_by_pid(pid_t pid);

/**
 * @brief Non-zero: find_process_by_pid() should use the seam probe. */
extern int seam_find_by_pid_override;
#endif

/**
 * @brief Determine whether a process ID satisfies the iterator filter
 *
 * @param pid Process ID to evaluate
 * @param filter Filter criteria to apply
 * @return 1 if the process matches the filter, 0 otherwise
 *
 * A process matches when:
 * - no PID filter is set (filter->pid == 0), or
 * - its PID equals the filter PID, or
 * - include_children is enabled and it is a descendant of the filter PID.
 *
 * Returns 0 when filter is NULL.
 */
int process_matches_filter(pid_t pid, const struct process_filter *filter);

/**
 * @brief Get the start time of a single process, if available
 *
 * @param pid Process ID to query
 * @return The process start time in seconds, or UNKNOWN_START_TIME when the
 *         process does not exist or the platform could not provide a value.
 *
 * A thin wrapper over the process iterator that returns just the start_time
 * field for one PID. Used to confirm a PID still belongs to the same process
 * before acting on it (e.g. sending a deferred signal to a PID that may have
 * been recycled). Callers must treat UNKNOWN_START_TIME as "cannot compare"
 * and fall back to acting on the PID.
 */
double get_process_start_time(pid_t pid);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PROCESS_ITERATOR_H */
