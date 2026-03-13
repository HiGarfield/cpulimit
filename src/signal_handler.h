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

#ifndef CPULIMIT_SIGNAL_HANDLER_H
#define CPULIMIT_SIGNAL_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/**
 * @brief Set up signal handlers for graceful program termination
 *
 * Registers a unified signal handler for SIGINT (Ctrl+C), SIGQUIT (Ctrl+\),
 * SIGTERM, SIGHUP, and SIGPIPE signals. When any of these signals are
 * received, the handler sets a quit flag that can be checked via
 * is_quit_flag_set(). For terminal-originated signals (SIGINT, SIGQUIT),
 * also sets a flag indicating TTY termination. The handler uses SA_RESTART
 * to automatically restart interrupted system calls.
 *
 * All signals are blocked at function entry (sigfillset + sigprocmask are
 * the very first operations) so that no termination signal can be delivered
 * before reset_signal_state() or the sigaction loop. The internal
 * signal-latch state (quit_flag, tty_quit_flag, quit_signal_num) is then
 * cleared, new handlers are installed, and the original mask is restored.
 * Any signal that becomes pending during the blocked window is delivered
 * through the new handlers once the mask is restored.
 *
 * @note Exits with error if signal mask or handler registration fails
 */
void configure_signal_handler(void);

/**
 * @brief Check if a termination signal has been received
 * @return 1 if a termination signal was caught, 0 otherwise
 *
 * Returns the state of the quit flag, which is set by the signal handler
 * when SIGINT, SIGQUIT, SIGTERM, or SIGHUP is received. The main program
 * loop should periodically check this flag to initiate graceful shutdown.
 */
int is_quit_flag_set(void);

/**
 * @brief Check if termination was triggered by terminal keyboard input
 * @return 1 if terminated by SIGINT or SIGQUIT, 0 otherwise
 *
 * Distinguishes between terminal-originated termination (Ctrl+C or Ctrl+\)
 * and other termination signals (SIGTERM, SIGHUP). This can be used to
 * customize shutdown behavior or messages based on how termination occurred.
 */
int is_terminated_by_tty(void);

/**
 * @brief Get the signal number that caused the quit flag to be set
 * @return Signal number (e.g. SIGTERM, SIGINT) of the first received
 *         termination signal, or 0 if no termination signal has been
 *         received yet
 *
 * Returns the signal number recorded when the first termination signal
 * was delivered to the process. This allows callers to forward the exact
 * received signal to child processes, ensuring consistent behavior with
 * a standard shell (e.g., Ctrl+C sends SIGINT to the child, not SIGTERM).
 */
int get_quit_signal(void);

/**
 * @brief Reset all signal handlers installed by configure_signal_handler()
 *        back to their default dispositions (SIG_DFL)
 * @return 0 on success, -1 on failure (errno set; error logged to stderr)
 *
 * Resets SIGINT, SIGQUIT, SIGTERM, SIGHUP, and SIGPIPE to SIG_DFL.
 */
int reset_signal_handlers_to_default(void);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_SIGNAL_HANDLER_H */
