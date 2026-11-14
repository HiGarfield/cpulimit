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

#ifndef CPULIMIT_SIGNAL_FORWARD_H
#define CPULIMIT_SIGNAL_FORWARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @brief Send a signal to the command, falling back to its own PID.
 *
 * @param child_pid PID of the command and of the group created for it.
 * @param sig Signal number to send.
 * @param verbose Non-zero to report a failed delivery.
 *
 * The group form is tried first so descendants are reached too; a failed
 * group delivery falls back to the child PID, which stays reachable even
 * when the command has left the group or a zombie blocks the group signal.
 * Non-positive PIDs and PID 1 are ignored.
 */
void signal_command(pid_t child_pid, int sig, int verbose);

/**
 * @brief Forward the received quit signal to the child process group.
 *
 * @param child_pid PID of the command and of its process group.
 * @param verbose Non-zero to report a failed delivery.
 *
 * The recorded signal is forwarded so the command exits with 128 + signal.
 * A zero recorded signal and SIGPIPE are mapped to SIGTERM instead.
 */
void forward_quit_signal(pid_t child_pid, int verbose);

#ifdef __cplusplus
}
#endif

#endif
