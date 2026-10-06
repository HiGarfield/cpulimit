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

#include "signal_forward.h"

#include "signal_handler.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Send a signal to the command, falling back to its own PID
 *
 * @param child_pid PID of the command; also the ID of the process group
 *                  created for it
 * @param sig Signal number to send
 * @param verbose Non-zero to report a delivery that failed; without it the
 *                failure is silent, because nothing here ends the run
 *
 * The negative-PID form is tried first so that descendants the limiter
 * cannot wait for are reached as well. It is not a reliable way to reach
 * the command itself, however:
 * - ESRCH once the command has moved itself into another process group.
 * - EPERM on macOS when no member of the group can be signalled, which a
 *   zombie still awaiting reap is enough to cause.
 *
 * Neither says anything about the command, which is this process's own
 * child and stays reachable by PID. Dropping the signal instead leaves a
 * suspended command suspended: it never resumes to take the forwarded
 * quit signal, so the only thing left to the caller is the SIGKILL
 * escalation, which reports the command as killed (128 + SIGKILL) rather
 * than as having exited on its own.
 */
void signal_command(pid_t child_pid, int sig, int verbose) {
    /*
     * The negative-PID form signals the whole process group, which is only
     * ours while child_pid is still the group leader. exec_child_process()
     * creates that group, but the program it execs is free to move itself
     * to another one, and once the child exits the id can be handed to an
     * unrelated group. Sending to -child_pid without looking first would
     * then reach processes that were never part of this run, which matters
     * most for the SIGKILL escalation.
     *
     * Verify leadership before using the group form; otherwise, and if the
     * group form fails for any reason, address the process directly.
     */
    if (child_pid > 0 && getpgid(child_pid) == child_pid &&
        kill(-child_pid, sig) == 0) {
        return;
    }
    if (kill(child_pid, sig) != 0 && errno != ESRCH) {
        int err = errno;
        /*
         * A delivery that does not land is not fatal: the caller goes on to
         * wait for the command or to escalate to SIGKILL, so the line is
         * only of interest to a run that asked to be narrated.
         */
        if (verbose) {
            fprintf(stderr, "kill(%ld, %d) failed: %s\n", (long)child_pid, sig,
                    strerror(err));
        }
    }
}

/**
 * @brief Forward the received quit signal to the child process group
 *
 * @param child_pid PID of the command; also the ID of its process group
 * @param verbose Non-zero to report a delivery that failed; without it the
 *                failure is silent, because a forwarded signal that does not
 *                land is not what ends the run
 *
 * The exact signal that caused cpulimit to quit is forwarded so the
 * command exits with the status a shell would report (128 + signal
 * number). Two special cases:
 * - get_quit_signal() == 0: theoretically unreachable once the quit
 *   flag is set, because a signal must have been recorded to set it;
 *   guarded defensively.
 * - SIGPIPE: an internal broken-pipe signal relevant only to the
 *   writing process; forwarding it could terminate children that write
 *   to unrelated pipes.
 * Both are mapped to SIGTERM so the child group is asked to exit
 * gracefully. The process group is targeted first, the command itself
 * as a fallback; see signal_command().
 */
void forward_quit_signal(pid_t child_pid, int verbose) {
    int fwd_sig;
    fwd_sig = get_quit_signal();
    if (fwd_sig == SIGPIPE || fwd_sig == 0) {
        fwd_sig = SIGTERM;
    }
    signal_command(child_pid, fwd_sig, verbose);
}
