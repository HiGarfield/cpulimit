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

#ifndef CPULIMIT_PROCESS_FINDER_H
#define CPULIMIT_PROCESS_FINDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @brief Check whether a process exists and can be controlled.
 *
 * @param pid PID to probe.
 * @return pid when kill(pid, 0) succeeds, -pid on EPERM or EACCES, 0 when
 *         the PID is invalid, absent or PID 1.
 */
pid_t find_process_by_pid(pid_t pid);

/**
 * @brief Find a running process by executable name or path.
 *
 * @param process_name Name or absolute path of the executable.
 * @return Positive PID when found and accessible, negative -PID when
 *         permission is denied, 0 when not found or on a failed scan.
 *
 * Compares against argv[0], in full for an absolute argument and otherwise
 * by basename. Among matches the topmost ancestor wins, then the smallest
 * PID; a controllable match is preferred and a vanished winner falls back.
 *
 * @note Scans every process; prefer find_process_by_pid() when the PID is
 *       known. A failed close is reported but does not discard the result.
 */
pid_t find_process_by_name(const char *process_name);

/**
 * @brief Compare a target name against one process's argv[0].
 *
 * @param process_name Name or absolute path as typed by the operator.
 * @param command argv[0] as reported by the OS.
 * @return 1 when the two name the same executable, 0 otherwise.
 *
 * An absolute argument is compared in full; any other name by basename.
 * Shared by the finder, the PID-reuse check and cli.c's init-name check so
 * all three agree on what counts as a match.
 */
int process_name_matches_cmd(const char *process_name, const char *command);

/**
 * @brief Check whether any process other than PID 1 carries this name.
 *
 * @param process_name Name or absolute path as typed by the operator.
 * @return 1 when a non-init match exists, 0 when a clean, completed scan
 *         proves none does, -1 when the answer cannot be established.
 *
 * @note Only a completed scan closed cleanly may answer 0, because cli.c
 *       refuses the name on that value.
 */
int process_name_has_non_init_match(const char *process_name);

/**
 * @brief Check whether a PID now runs a different executable.
 *
 * @param pid PID to inspect.
 * @param process_name Expected executable name or path.
 * @return 1 only when a different executable is confirmed, 0 otherwise.
 */
int process_has_other_name(pid_t pid, const char *process_name);

#ifdef __cplusplus
}
#endif

#endif
