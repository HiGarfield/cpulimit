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

#ifndef CPULIMIT_CHILD_WAIT_H
#define CPULIMIT_CHILD_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cli.h"

#include <sys/types.h>

/**
 * @brief Wait for the child process to exit and collect its exit status
 * @return The child's exit status if successfully reaped, EXIT_FAILURE
 *         otherwise
 *
 * Polls for the child's termination. Once the quit signal has been
 * forwarded, a 5-second SIGKILL timeout is applied if the child does not
 * exit in time. Translates signal termination to shell-compatible exit
 * codes (128 + signal number).
 *
 * Handles the race where a quit signal is delivered after
 * run_command_mode()'s is_quit_flag_set() check: if quit_flag becomes set
 * during polling, the exact received signal is forwarded to the child
 * process group so it exits with the correct status (128 + signal number)
 * rather than waiting for the SIGKILL timeout.  A SIGCONT is always sent
 * first so that a stopped child (e.g. on macOS 10.7 where stopped processes
 * are invisible to the process iterator) is resumed before the forwarded
 * signal is delivered.
 *
 * The signal is delivered exactly once.  When run_command_mode() has
 * already forwarded it (the common case, since the quit flag is normally
 * set before this function is reached) this function must not send a
 * second one: two SIGINTs from a single Ctrl+C would cut short the
 * handler a program installs to shut itself down cleanly, and no shell
 * behaves that way.
 *
 * start_time is reset to the moment the signal is forwarded from inside
 * this function, giving the child the full CHILD_KILL_TIMEOUT_MS from the
 * point it first receives the forwarded signal (not from function entry).
 * No escalation is armed before that: a command that is still running
 * because limit_process() stopped watching it is waited for, not killed.
 *
 * @param child_pid PID of the child process to wait for
 * @param cfg Pointer to configuration structure (used for verbose output)
 * @param signal_forwarded Non-zero if the caller already forwarded the
 *                         quit signal to the child process group
 */
int collect_child_exit_status(pid_t child_pid, const struct cpulimit_cfg *cfg,
                              volatile int signal_forwarded);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CHILD_WAIT_H */
