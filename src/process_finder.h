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
 * @brief Check if a process exists and can be controlled by cpulimit
 *
 * @param pid Process ID to search for
 * @return Positive PID if process exists and can be signaled
 *         (kill(pid,0)==0), negative -PID if it exists but permission is
 *         denied (EPERM/EACCES), 0 if it does not exist or the PID is invalid
 */
pid_t find_process_by_pid(pid_t pid);

/**
 * @brief Find a running process by its executable name or path
 *
 * @param process_name Name or absolute path of the executable to search for
 * @return Positive PID if found and accessible, negative -PID if found but
 *         permission denied, 0 if not found or invalid name
 *
 * Compares against argv[0] (full path if absolute, else basename). With
 * several matches the topmost ancestor wins, and among unrelated ones the
 * smallest PID wins; a controllable match is preferred over an uncontrollable
 * one, and a vanished winner falls back to another live candidate.
 *
 * @note Iterates every process, so prefer find_process_by_pid() when the PID is
 *       known; returns 0 on allocation or iterator errors.
 */
pid_t find_process_by_name(const char *process_name);

/**
 * @brief Check whether a PID has since been taken over by another program
 *
 * @param pid Process ID to inspect
 * @param process_name Executable name or absolute path expected for that PID
 * @return 1 only when the PID is confirmed running a different executable, 0
 *         when it matches or no conclusion can be drawn
 *
 * Closes the window between resolving a name to a PID and starting to limit it;
 * the test is one-sided so any uncertainty keeps prior behavior unchanged.
 */
int process_has_other_name(pid_t pid, const char *process_name);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PROCESS_FINDER_H */
