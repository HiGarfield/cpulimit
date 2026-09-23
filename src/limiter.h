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
 * @param cfg Pointer to configuration structure containing command and options
 *
 * This function implements command execution mode (COMMAND [ARG]...):
 * 1. Forks a child process to execute the specified command
 * 2. Creates a new process group for the child
 * 3. Applies CPU limiting to the command and optionally its descendants
 * 4. Waits for command completion and exits with the child's exit status
 *
 * The parent process monitors the child and handles:
 * - Normal exit (returns child's exit code)
 * - Signal termination (returns 128 + signal number)
 * - Timeout after termination request (sends SIGKILL)
 *
 * @return Exit status code; the caller is responsible for calling exit()
 */
int run_command_mode(const struct cpulimit_cfg *cfg);

/**
 * @brief Search for and limit an existing process by PID or executable name
 * @param cfg Pointer to configuration structure containing target specification
 *
 * This function implements PID/exe search mode (-p PID or -e EXE):
 * 1. Continuously searches for the target process
 * 2. When found, applies CPU limiting
 * 3. Behavior depends on lazy_mode flag:
 *    - lazy_mode=1: one attempt, then exit with a failure if the attempt did
 *      not end because the target terminated
 *    - lazy_mode=0: a watch that lasts as long as the run.  Every attempt
 *      ends by searching again, so nothing about the target ends it: not
 *      being started yet, exiting (while running or while suspended by this
 *      run), reappearing on a recycled PID, and refusing every signal all
 *      keep it looking, because the target can come back and must be limited
 *      again when it does.  What does end it is a failure of the scanning
 *      machinery (allocation, clock or process-iterator initialisation),
 *      after which nothing can be searched with, or an attempt that left a
 *      member stopped and needing repair by hand
 * 4. A target that turns out to be cpulimit itself is refused outright in
 *    both modes, with a failure exit status
 *
 * @return Exit status code; the caller is responsible for calling exit()
 */
int run_pid_or_exe_mode(const struct cpulimit_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_LIMITER_H */
