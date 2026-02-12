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

#ifndef __LIMITER_H
#define __LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli.h"

/**
 * @brief Run the program in command execution mode
 * @param cfg Pointer to the configuration structure
 * @note This function forks a child process to execute a command and then
 *       limits the CPU usage of that command and its children.
 */
void run_command_mode(const struct cpulimitcfg *cfg);

/**
 * @brief Run the program in PID or executable name mode
 * @param cfg Pointer to the configuration structure
 * @note This function continuously searches for the target process (by PID or
 *       executable name) and applies CPU limiting when found.
 */
void run_pid_or_exe_mode(const struct cpulimitcfg *cfg);

#ifdef __cplusplus
}
#endif

#endif
