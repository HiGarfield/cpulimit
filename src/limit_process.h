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

#ifndef CPULIMIT_LIMIT_PROCESS_H
#define CPULIMIT_LIMIT_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @def LIMIT_PROCESS_OK
 * @brief limit_process() completed: the target terminated or a quit signal
 *        was received, and every suspended process has been resumed
 */
#define LIMIT_PROCESS_OK 0

/**
 * @def LIMIT_PROCESS_ERROR
 * @brief limit_process() could not start limiting
 *
 * The process group could not be set up (allocation, clock or process-scan
 * failure).  Nothing was stopped, so there is nothing to resume; the
 * caller decides what to do and must not treat the target as limited.
 */
#define LIMIT_PROCESS_ERROR (-1)

/**
 * @brief Enforce CPU usage limit on a process or process set
 * @param pid Process ID of the target process to limit
 * @param cpu_limit CPU usage limit expressed in CPU cores (core
 *              equivalents), in the range (0, N_CPU]. Example: on a 4-core
 *              system, cpu_limit=0.5 means 50% of one core (12.5% of total
 *              capacity), and cpu_limit=2.0 means two full cores (50% of
 *              total capacity).
 * @param include_children If non-zero, limit applies to target and all
 *                         descendants; if zero, limit applies only to target
 *                         process
 * @param verbose If non-zero, print periodic statistics about CPU usage and
 *                control; if zero, operate silently
 *
 * This function implements the core CPU limiting algorithm using
 * SIGSTOP/SIGCONT:
 * 1. Monitors the process set's actual CPU usage
 * 2. Calculates appropriate work/sleep intervals to achieve the target limit
 * 3. Alternately sends SIGCONT (allow execution) and SIGSTOP (suspend
 * execution)
 * 4. Dynamically adjusts timing based on measured CPU usage
 * 5. Continues until the target terminates or quit signal received
 *
 * @note This function blocks until target terminates or is_quit_flag_set()
 *       returns true
 * @note Always resumes suspended processes (sends SIGCONT) before returning
 *
 * @return LIMIT_PROCESS_OK once limiting has finished, LIMIT_PROCESS_ERROR
 *         if the process group could not be initialised.  On error nothing
 *         has been stopped, so the caller is free to resume/reap its own
 *         child; the previous behaviour of exiting the whole process here
 *         left a command-mode child running unthrottled and unreaped.
 */
int limit_process(pid_t pid, double cpu_limit, int include_children,
                  int verbose);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_LIMIT_PROCESS_H */
