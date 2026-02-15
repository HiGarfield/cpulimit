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

#ifndef __LIMIT_PROCESS_H
#define __LIMIT_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>

/**
 * @brief Enforce CPU usage limit on a process or process group
 * @param pid Process ID of the target process to limit
 * @param limit CPU usage limit expressed in CPU cores (core equivalents), in
 *              the range (0, N_CPU]. Example: on a 4-core system,
 *              limit=0.5 means 50% of one core (12.5% of total capacity),
 *              and limit=2.0 means two full cores (50% of total capacity).
 * @param include_children If non-zero, limit applies to target and all
 *                         descendants; if zero, limit applies only to target
 * process
 * @param verbose If non-zero, print periodic statistics about CPU usage and
 *                control; if zero, operate silently
 *
 * This function implements the core CPU limiting algorithm using
 * SIGSTOP/SIGCONT:
 * 1. Monitors the process group's actual CPU usage
 * 2. Calculates appropriate work/sleep intervals to achieve the target limit
 * 3. Alternately sends SIGCONT (allow execution) and SIGSTOP (suspend
 * execution)
 * 4. Dynamically adjusts timing based on measured CPU usage
 * 5. Continues until the target terminates or quit signal received
 *
 * @note This function blocks until target terminates or is_quit_flag_set()
 *       returns true
 * @note Always resumes suspended processes (sends SIGCONT) before returning
 */
void limit_process(pid_t pid, double limit, int include_children, int verbose);

#ifdef __cplusplus
}
#endif

#endif
