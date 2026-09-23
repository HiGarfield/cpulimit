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
 * @param pid Process ID to search for
 * @return Positive PID if process exists and can be signaled
 *         (kill(pid,0)==0), negative -PID if process exists but permission
 *         denied (errno==EPERM), 0 if process does not exist (errno==ESRCH or
 *         invalid PID)
 *
 * Uses kill(pid, 0) as a lightweight probe to test process existence and
 * signal permission without actually sending a signal. This is the standard
 * POSIX method for checking process liveness and accessibility.
 */
pid_t find_process_by_pid(pid_t pid);

/**
 * @brief Find a running process by its executable name or path
 * @param process_name Name or absolute path of the executable to search for
 * @return Positive PID if found and accessible, negative -PID if found but
 *         permission denied, 0 if not found or invalid name
 *
 * A name is compared against argv[0]: an absolute path is compared in full,
 * a bare or relative name only by its basename.
 *
 * When several processes match, the choice is deterministic and independent
 * of the platform's iteration order: the topmost ancestor wins, and among
 * unrelated matches the smallest PID wins.  If the chosen process exits
 * before the existence recheck, the best surviving candidate is selected by
 * the same rule.  A match cpulimit cannot signal is never preferred over one
 * it can, so a negative PID is returned only when every match is
 * uncontrollable.
 *
 * @note Returns 0 immediately for NULL or empty process_name
 * @note Iterates every process on the system, which is slow when there are
 *       many; prefer find_process_by_pid() when the PID is known
 * @note On critical errors (allocation or iterator initialization failure)
 *       returns 0, so the caller can treat the target as "not found" instead
 *       of aborting the run; a failure to close the iterator is reported but
 *       does not change the selection
 */
pid_t find_process_by_name(const char *process_name);

/**
 * @brief Check whether a PID has since been taken over by another program
 * @param pid Process ID to inspect
 * @param process_name Executable name or absolute path expected for that PID
 * @return 1 only when the PID is positively identified as running a
 *         different executable, 0 when it matches or when no conclusion can
 *         be drawn
 *
 * Used to close the window between resolving a name to a PID and starting
 * to limit it: if the original process exited and the PID was recycled, the
 * name no longer matches and the PID must not be touched.
 *
 * The test is deliberately one-sided.  Anything that prevents a conclusion
 * -- the process is gone, /proc cannot be read, no name was supplied -- is
 * reported as "no mismatch", so callers keep behaving exactly as they did
 * before; only a confirmed mismatch stops them.
 */
int process_has_other_name(pid_t pid, const char *process_name);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PROCESS_FINDER_H */
