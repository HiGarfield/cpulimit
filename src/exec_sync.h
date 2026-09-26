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

#ifndef CPULIMIT_EXEC_SYNC_H
#define CPULIMIT_EXEC_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @brief Wait for the child process to complete exec setup
 *
 * @param child_pid PID of the forked child process
 * @param sync_read_fd Read end of the synchronization pipe
 *
 * Reads the readiness byte written by the child after setpgid() and signal
 * handler reset, then blocks until the pipe EOF that proves exec completed (or
 * the child exited on exec failure). Closes sync_read_fd on return.
 *
 * @return 0 on success, -1 on error (the child is killed and reaped)
 */
int wait_for_child_exec(pid_t child_pid, int sync_read_fd);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_EXEC_SYNC_H */
