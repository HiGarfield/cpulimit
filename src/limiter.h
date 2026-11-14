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

#ifndef CPULIMIT_LIMITER_H
#define CPULIMIT_LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cli.h"

/**
 * @brief Execute and monitor a command with CPU limiting.
 *
 * @param cfg Configuration with the command and options.
 * @return The process exit status.
 *
 * Forks the child into its own process group, applies the limit and waits
 * for completion, escalating to SIGKILL after a termination timeout.
 */
int run_command_mode(const struct cpulimit_cfg *cfg);

/**
 * @brief Find and limit an existing process by PID or executable name.
 *
 * @param cfg Configuration naming the target.
 * @return The process exit status.
 *
 * Lazy mode makes one attempt. Otherwise the run keeps re-resolving and
 * re-limiting until a group-building failure or a stranded member ends it;
 * a scan failure after the group is built is reported once per streak and
 * retried. cpulimit itself is never a target.
 *
 * @note A PID target always runs in lazy mode, even without -z.
 */
int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif
