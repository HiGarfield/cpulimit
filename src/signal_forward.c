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
void signal_command(pid_t child_pid, int sig, int verbose) {
    /* Only a positive PID other than 1 is safe to signal. */
    if (child_pid <= 1) {
        return;
    }
    /* Verify the child still leads its group before kill(-pid, ...). */
    /* The check and the kill leave a race POSIX cannot close. */
    if (getpgid(child_pid) == child_pid && kill(-child_pid, sig) == 0) {
        return;
    }
    if (kill(child_pid, sig) != 0 && errno != ESRCH) {
        int err = errno;
        if (verbose) {
            fprintf(stderr, "kill(%ld, %d) failed: %s\n", (long)child_pid, sig,
                    strerror(err));
        }
    }
}

/**
 * @brief Forward the received quit signal to the child process group.
 *
 * @param child_pid PID of the command and of its process group.
 * @param verbose Non-zero to report a failed delivery.
 *
 * The recorded signal is forwarded so the command exits with 128 + signal.
 * A zero recorded signal and SIGPIPE are mapped to SIGTERM instead.
 */
void forward_quit_signal(pid_t child_pid, int verbose) {
    int fwd_sig;
    fwd_sig = get_quit_signal();
    if (fwd_sig == SIGPIPE || fwd_sig == 0) {
        fwd_sig = SIGTERM;
    }
    signal_command(child_pid, fwd_sig, verbose);
}
