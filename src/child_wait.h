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
 * @brief Wait for the child to exit and collect its exit status
 * @param child_pid PID of the child process to wait for
 * @param cfg Pointer to configuration structure (used for verbose output)
 * @param signal_forwarded Non-zero if the caller already forwarded the quit
 *                         signal to the child process group
 * @return The child's exit status if it was reaped, EXIT_FAILURE otherwise
 *
 * Polls until the child exits, translating death by signal into the
 * shell-compatible 128 + signal number.  Once the quit signal has been
 * forwarded, a child that still has not exited after CHILD_KILL_TIMEOUT_MS is
 * killed, and that escalation fires once rather than once per poll: a child
 * that has exited but not yet been reaped is a zombie that still carries its
 * PID and PGID, so repeating the group kill would reach whatever else now
 * lives in that group.  The grace period starts when the signal is forwarded,
 * not at entry, so the child gets its full timeout measured from the moment
 * it first sees the signal; nothing is armed before that, because a command
 * that is merely not being watched is waited for rather than killed.
 *
 * A quit signal that arrives after run_command_mode()'s own check is handled
 * by forwarding the exact received signal -- but only once, since the caller
 * normally forwards first and a second SIGINT from one Ctrl+C would cut short
 * a handler a program installs to shut itself down cleanly.  SIGCONT is sent
 * first so a stopped child is resumed before the signal is delivered; on
 * macOS 10.7 a stopped process is invisible to the process iterator.
 *
 * An internal failure (an unreadable clock) does not end the process: the
 * child is resumed, waited for so it does not stay a zombie, and EXIT_FAILURE
 * is returned while the caller still produces its own diagnosis.
 */
int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              int signal_forwarded);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CHILD_WAIT_H */
