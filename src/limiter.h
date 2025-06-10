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

#ifndef __LIMITER_H
#define __LIMITER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli.h"

/**
 * Runs the limiter in command mode.
 * Executes the specified command and limits its CPU usage.
 *
 * @param cfg Pointer to the configuration struct.
 */
void run_command_mode(const struct cpulimitcfg *cfg);

/**
 * Runs the limiter in pid or exe mode.
 * Continuously searches for the target process and limits its CPU usage.
 * The limiter will exit if the target process is not found or if the quit flag
 * is set.
 *
 * @param cfg Pointer to the configuration struct.
 */
void run_pid_or_exe_mode(const struct cpulimitcfg *cfg);

#endif
