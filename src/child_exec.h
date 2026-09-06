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

#ifndef CPULIMIT_CHILD_EXEC_H
#define CPULIMIT_CHILD_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cli.h"

/**
 * @brief Execute a child process for command mode
 * @param cfg Pointer to configuration structure containing command and options
 * @param sync_read_fd Read end of the synchronization pipe
 * @param sync_write_fd Write end of the synchronization pipe
 *
 * This function executes in the child process after fork(). It sets up the
 * process group, resets signal handlers, signals readiness to the parent,
 * and replaces the process image with the user command via execvp().
 *
 * @note This function never returns; it calls _exit() on any failure
 */
void exec_child_process(const struct cpulimit_cfg *cfg, int sync_read_fd,
                        int sync_write_fd);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CHILD_EXEC_H */
