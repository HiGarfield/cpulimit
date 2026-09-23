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
 * @brief limit_process() could not start limiting
 *
 * The process group could not be set up (allocation, clock or process-scan
 * failure), or limiting ran to its end and then failed to resume a suspended
 * member.  Nothing was stopped, so there is nothing to resume; the caller
 * decides what to do and must not treat the target as limited.
 *
 * Only when the scan failure is NOT also present: a run that both stopped on
 * a failed scan and left a member suspended returns
 * LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED instead, so the caller can describe
 * both facts instead of only the stranded one.
 */
#define LIMIT_PROCESS_ERROR (-1)

/**
 * @def LIMIT_PROCESS_SCAN_FAILED
 * @brief limit_process() had to stop limiting because a process scan failed
 *        while the control loop was running
 *
 * The group was built and limiting did run, but the per-cycle process-group
 * scan later failed, so the control loop stopped and limit_process()
 * resumed every suspended member before returning.  Nothing is being
 * limited from that point on and nothing is stranded, so this is neither
 * LIMIT_PROCESS_OK (the run did not limit anything to completion) nor
 * LIMIT_PROCESS_ERROR (nothing has to be repaired by the caller).
 *
 * Only when every member was resumed: if one could not be, the run returns
 * LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED instead, because it is both.
 *
 * limit_process() reports the reason on stderr itself.  A caller that can
 * re-resolve its target -- non-lazy -p/-e mode -- may simply try again.
 * Every other caller has no second chance and must report a failure:
 * command mode, and lazy mode (-p, or -e together with -z), whose whole
 * point is to stop as soon as the target is gone instead of re-attaching
 * to it.  For those a limit that stopped mid-run is final: the target is
 * no longer limited and nothing will pick it up again.
 */
#define LIMIT_PROCESS_SCAN_FAILED 1

/**
 * @def LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED
 * @brief limit_process() had to stop limiting on a failed scan AND could not
 *        resume every member afterwards
 *
 * Both of the above hold at once: the control loop stopped on a failed scan,
 * so nothing is limited any more, and at least one member is still stopped
 * and needs 'kill -CONT <pid>' by hand.
 *
 * Reporting this as LIMIT_PROCESS_ERROR would describe a run that did limit
 * for a while as one that never applied the limit at all, which sends anyone
 * debugging it after permissions or target resolution instead of the failed
 * scan.  limit_process() has already named every stranded PID on stderr by
 * the time it returns this, so a caller only has to add that limiting stopped
 * early.
 */
#define LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED 2

/**
 * @brief Enforce CPU usage limit on a process or process set
 * @param pid Process ID of the target process to limit
 * @param cpu_limit CPU usage limit in core equivalents, in the range
 *              (0, N_CPU]: on a 4-core system, 0.5 means half of one core and
 *              2.0 means two full cores
 * @param include_children If non-zero, limit applies to target and all
 *                         descendants; if zero, limit applies only to target
 *                         process
 * @param verbose If non-zero, print periodic statistics about CPU usage and
 *                control; if zero, operate silently
 * @param prior_scan_failures How many consecutive scan failures the caller
 *        has already recorded for this run.  A caller that retries passes
 *        its streak so the per-cycle scan diagnostic is printed only on the
 *        first failure of the streak instead of once per retry; a caller
 *        that runs once passes 0.  It does not change what is returned.
 *
 * Implements the core CPU limiting algorithm using SIGSTOP/SIGCONT:
 * 1. Monitors the process set's actual CPU usage
 * 2. Calculates appropriate work/sleep intervals to achieve the target limit
 * 3. Alternately sends SIGCONT (allow execution) and SIGSTOP (suspend
 *    execution)
 * 4. Dynamically adjusts timing based on measured CPU usage
 * 5. Continues until the target terminates or a quit signal is received
 *
 * @note This function blocks until the target terminates or
 *       is_quit_flag_set() returns true
 * @note Every suspended process is resumed (SIGCONT) before returning; any
 *       that could not be is named on stderr with the command to recover it,
 *       so no caller has to repeat that
 *
 * @return LIMIT_PROCESS_OK when limiting finished with everything resumed,
 *         LIMIT_PROCESS_SCAN_FAILED or LIMIT_PROCESS_SCAN_FAILED_AND_STRANDED
 *         when the control loop stopped on a failed process-group scan, and
 *         LIMIT_PROCESS_ERROR when the group could not be initialised or a
 *         member could not be resumed.  Each macro documents what its value
 *         means for the caller.
 */
int limit_process(pid_t pid, double cpu_limit, int include_children,
                  int verbose, unsigned int prior_scan_failures);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_LIMIT_PROCESS_H */
