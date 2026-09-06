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

void signal_command(pid_t child_pid, int sig) {
    if (kill(-child_pid, sig) == 0) {
        return;
    }
    if (kill(child_pid, sig) != 0 && errno != ESRCH) {
        fprintf(stderr, "kill(%ld, %d) failed: %s\n", (long)child_pid, sig,
                strerror(errno));
    }
}

void forward_quit_signal(pid_t child_pid) {
    int fwd_sig;
    fwd_sig = get_quit_signal();
    if (fwd_sig == SIGPIPE || fwd_sig == 0) {
        fwd_sig = SIGTERM;
    }
    signal_command(child_pid, fwd_sig);
}
