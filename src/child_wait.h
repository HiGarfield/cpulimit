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

#ifndef CPULIMIT_CHILD_WAIT_H
#define CPULIMIT_CHILD_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cli.h"

#include <sys/types.h>

/**
 * @brief Resume and reap the command's child, without blocking on it
 *
 * @param child_pid PID of the child process to reap
 *
 * For failure paths that return EXIT_FAILURE: the child is still out there, so
 * resume it (the caller already sent SIGCONT) and reap it with WNOHANG -- a
 * blocking wait would hang on a child that ignores the signal, since this path
 * skips the polling loop's SIGKILL escalation. A child that has not exited is
 * reparented to init.
 */
void reap_child_before_error_return(pid_t child_pid);

/**
 * @brief Wait for the child to exit and collect its exit status
 *
 * @param child_pid PID of the child process to wait for
 * @param cfg Pointer to configuration structure (used for verbose output)
 * @param signal_forwarded Non-zero if the caller already forwarded the quit
 *                         signal to the child process group
 * @return The child's exit status if reaped, EXIT_FAILURE otherwise
 *
 * Polls with WNOHANG, translating death by signal into 128 + signal. Once the
 * quit signal is forwarded, escalates to SIGKILL after CHILD_KILL_TIMEOUT_MS
 * (once, on the group); a late quit signal is forwarded here at most once. An
 * unreadable clock does not end the process: the child is resumed and reaped
 * first, then EXIT_FAILURE is returned.
 */
int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              int signal_forwarded);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CHILD_WAIT_H */
