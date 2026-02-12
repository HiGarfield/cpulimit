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
 * @brief Apply CPU usage limiting to a process or process group
 * @param pid Process ID of the target process
 * @param limit CPU usage limit (0.0 to N_CPU)
 * @param include_children Flag to include child processes
 * @param verbose Verbose output flag
 * @note This function implements the main CPU limiting algorithm that
 *       alternates between letting the target process run and stopping
 *       it to enforce the CPU usage limit.
 */
void limit_process(pid_t pid, double limit, int include_children, int verbose);

#ifdef __cplusplus
}
#endif

#endif
