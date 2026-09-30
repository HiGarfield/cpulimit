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
 * @brief Execute and monitor a user-specified command with CPU limiting
 *
 * @param cfg Pointer to configuration with command and options
 * @return Exit status code; the caller is responsible for calling exit()
 *
 * Forks a child into its own process group, applies the limit, and waits for
 * completion, sending SIGKILL after a termination-request timeout.
 */
int run_command_mode(const struct cpulimit_cfg *cfg);

/**
 * @brief Search for and limit an existing process by PID or executable name
 *
 * @param cfg Pointer to configuration with the target specification
 * @return Exit status code; the caller is responsible for calling exit()
 *
 * In lazy mode one attempt is made and the program exits on any non-target
 * exit. Otherwise it watches until a scan failure or a stranded member stops
 * it; a target reappearing on a recycled PID is re-limited. cpulimit itself is
 * always refused as a target.
 */
int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_LIMITER_H */
