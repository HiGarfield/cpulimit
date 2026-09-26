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
 * @brief limit_process() completed: the target terminated or a quit signal
 *        was received, and every suspended process has been resumed
 */
#define LIMIT_PROCESS_OK 0

/**
 * @def LIMIT_PROCESS_ERROR
 * @brief limit_process() could not start: the process group was never built
 *
 * Allocation, clock or process-scan failure meant nothing was ever stopped,
 * so there is nothing a caller can repair. This is the only outcome that says
 * the limit was never applied.
 */
#define LIMIT_PROCESS_ERROR (-1)

/**
 * @def LIMIT_PROCESS_SCAN_FAILED
 * @brief limit_process() stopped limiting because a per-cycle scan failed
 *
 * The group was built and limiting ran, but the control loop stopped and
 * resumed every member before returning, so nothing is stranded. Non-lazy
 * -p/-e callers may retry; command and lazy callers treat it as a final stop.
 */
#define LIMIT_PROCESS_SCAN_FAILED 1

/**
 * @def LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED
 * @brief limit_process() stopped on a failed scan AND could not resume everyone
 *
 * The control loop stopped on a failed scan and at least one member is still
 * stopped, needing 'kill -CONT <pid>' by hand. The stranded PIDs are already
 * named on stderr by limit_process().
 */
#define LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED 2

/**
 * @def LIMIT_PROCESS_STRANDED
 * @brief limit_process() ran to its end but could not resume every member
 *
 * The limit was applied; what failed is the shutdown resume round, leaving at
 * least one member stopped. Those PIDs are named on stderr. Retrying cannot
 * help: the member must be released by hand.
 */
#define LIMIT_PROCESS_STRANDED 3

/**
 * @brief Enforce a CPU usage limit on a process or process set
 *
 * @param pid Process ID of the target process to limit
 * @param cpu_limit CPU limit in core equivalents, range (0, N_CPU]
 * @param include_children Non-zero to limit descendants too, zero for target
 * only
 * @param verbose Non-zero to print periodic statistics
 * @param prior_scan_failures Consecutive scan failures the caller has already
 *        recorded, so the per-cycle diagnostic prints once per streak rather
 *        than per retry; it does not change the return value
 * @return One of the LIMIT_PROCESS_* codes: OK when finished with everything
 *         resumed; SCAN_FAILED (or _AND_STRANDED if not all resumed) on a
 * failed scan; STRANDED when the run ended but a member could not be resumed;
 *         ERROR when the group could not be built. Every non-OK value means
 * the target is no longer limited.
 *
 * @note Blocks until the target terminates or is_quit_flag_set() is true; every
 *       suspended process is resumed before returning.
 */
int limit_process(pid_t pid, double cpu_limit, int include_children,
                  int verbose, unsigned int prior_scan_failures);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_LIMIT_PROCESS_H */
