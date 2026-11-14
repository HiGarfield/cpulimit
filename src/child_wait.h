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
 * @brief Reap the child without blocking on an error path.
 *
 * @param child_pid PID of the child to reap.
 *
 * @note The caller sends SIGCONT; a child that is still running is left to
 *       be reparented.
 */
void reap_child_before_error_return(pid_t child_pid);

/**
 * @brief Wait for the child to exit and collect its exit status.
 *
 * @param child_pid PID of the child to wait for.
 * @param cfg Configuration, used for verbose output.
 * @param signal_forwarded Non-zero if the caller already forwarded the quit
 *        signal to the child's process group.
 * @return The child's exit status if reaped, EXIT_FAILURE otherwise.
 *
 * Polls with WNOHANG and maps death by signal to 128 + signal. After a quit
 * signal is forwarded, SIGKILL escalation is requested once, after
 * CHILD_KILL_TIMEOUT_MS. A clock failure resumes and reaps the child before
 * returning EXIT_FAILURE.
 */
int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              int signal_forwarded);

#ifdef __cplusplus
}
#endif

#endif
