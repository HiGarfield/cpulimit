/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifndef __LIMIT_PROCESS_H
#define __LIMIT_PROCESS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>

/**
 * Controls the CPU usage of a process (and optionally its children).
 * Limits the amount of time the process can run based on a given percentage.
 *
 * @param pid Process ID of the target process.
 * @param limit The CPU usage limit (0 to N_CPU).
 * @param include_children Whether to include child processes.
 * @param verbose Verbose mode flag.
 */
void limit_process(pid_t pid, double limit, int include_children, int verbose);

#endif
