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

#ifndef CPULIMIT_SIGNAL_FORWARD_H
#define CPULIMIT_SIGNAL_FORWARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @brief Send a signal to the command, falling back to its own PID
 * @param child_pid PID of the command; also the ID of the process group
 *                  created for it
 * @param sig Signal number to send
 *
 * The negative-PID form is tried first so that descendants the limiter
 * cannot wait for are reached as well.  It is not a reliable way to reach
 * the command itself, however:
 * - ESRCH once the command has moved itself into another process group.
 * - EPERM on macOS when no member of the group can be signalled, which a
 *   zombie still awaiting reap is enough to cause.
 *
 * Neither says anything about the command, which is this process's own
 * child and stays reachable by PID.  Dropping the signal instead leaves a
 * suspended command suspended: it never resumes to take the forwarded
 * quit signal, so the only thing left to the caller is the SIGKILL
 * escalation, which reports the command as killed (128 + SIGKILL) rather
 * than as having exited on its own.
 */
void signal_command(pid_t child_pid, int sig);

/**
 * @brief Forward the received quit signal to the child process group
 * @param child_pid PID of the command; also the ID of its process group
 *
 * The exact signal that caused cpulimit to quit is forwarded so the
 * command exits with the status a shell would report (128 + signal
 * number).  Two special cases:
 * - get_quit_signal() == 0: theoretically unreachable once the quit
 *   flag is set, because a signal must have been recorded to set it;
 *   guarded defensively.
 * - SIGPIPE: an internal broken-pipe signal relevant only to the
 *   writing process; forwarding it could terminate children that write
 *   to unrelated pipes.
 * Both are mapped to SIGTERM so the child group is asked to exit
 * gracefully.  The process group is targeted first, the command itself
 * as a fallback; see signal_command().
 */
void forward_quit_signal(pid_t child_pid);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_SIGNAL_FORWARD_H */
