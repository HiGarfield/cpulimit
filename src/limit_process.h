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

#ifndef CPULIMIT_LIMIT_PROCESS_H
#define CPULIMIT_LIMIT_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @def LIMIT_PROCESS_OK
 * @brief The run finished and every suspended member was resumed.
 */
#define LIMIT_PROCESS_OK 0

/**
 * @def LIMIT_PROCESS_ERROR
 * @brief The group could not be built, so nothing was ever suspended.
 */
#define LIMIT_PROCESS_ERROR (-1)

/**
 * @def LIMIT_PROCESS_SCAN_FAILED
 * @brief Limiting stopped on a per-cycle scan failure; all are resumed.
 *
 * @note A caller that re-resolves its target may retry; command and lazy
 *       callers end the run.
 */
#define LIMIT_PROCESS_SCAN_FAILED 1

/**
 * @def LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED
 * @brief Limiting stopped on a scan failure and a member is still stopped.
 */
#define LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED 2

/**
 * @def LIMIT_PROCESS_STRANDED
 * @brief The run ended but at least one member could not be resumed.
 */
#define LIMIT_PROCESS_STRANDED 3

/**
 * @def LIMIT_PROCESS_NO_TARGET
 * @brief The named PID was never limitable, so nothing was suspended.
 *
 * @note A zombie, a departed process, PID 1 or cpulimit itself leave the
 *       group empty. This is not a completed limit.
 */
#define LIMIT_PROCESS_NO_TARGET 4

/**
 * @brief Enforce a CPU usage limit on a process or process set.
 *
 * @param pid PID of the target.
 * @param cpu_limit Limit in core equivalents, in (0, N_CPU].
 * @param include_children Non-zero to include descendants.
 * @param verbose Non-zero to print statistics and per-attempt diagnostics.
 * @param prior_scan_failures Consecutive scan failures already recorded, so
 *        the scan diagnostic prints once per streak.
 * @return One of the LIMIT_PROCESS_* codes; every non-OK value means the
 *         target is no longer limited.
 *
 * @note Blocks until the target terminates or is_quit_flag_set() is true,
 *       and resumes suspended members before returning where possible.
 */
int limit_process(pid_t pid, double cpu_limit, int include_children,
                  int verbose, unsigned int prior_scan_failures);

#ifdef __cplusplus
}
#endif

#endif
