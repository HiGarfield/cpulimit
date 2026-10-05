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

#ifndef CPULIMIT_SIGNAL_HANDLER_H
#define CPULIMIT_SIGNAL_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the unified termination-signal handler
 *
 * Blocks all signals, clears the internal latch state, installs SA_RESTART
 * handlers for SIGINT/SIGQUIT/SIGTERM/SIGHUP/SIGPIPE, then restores the mask.
 *
 * @note Exits with error if the mask or registration fails
 */
void configure_signal_handler(void);

/**
 * @brief Check if a termination signal has been received
 *
 * @return 1 if a termination signal was caught, 0 otherwise
 */
int is_quit_flag_set(void);

/**
 * @brief Save the current terminal attributes of standard input
 *
 * Stores the terminal settings in internal state so they can be restored by
 * restore_terminal_attributes(). Does nothing when standard input is not a
 * terminal. Must be called before disable_terminal_echo().
 */
void save_terminal_attributes(void);

/**
 * @brief Disable terminal echo on standard input
 *
 * Clears the ECHO flag so the terminal driver does not echo typed characters
 * (most importantly, the "^C" of a Ctrl+C). Does nothing unless
 * save_terminal_attributes() succeeded.
 */
void disable_terminal_echo(void);

/**
 * @brief Restore the terminal attributes saved by save_terminal_attributes()
 *
 * Restores the original terminal settings on standard input. Does nothing
 * unless save_terminal_attributes() succeeded.
 */
void restore_terminal_attributes(void);

/**
 * @brief Get the signal number that set the quit flag
 *
 * @return Signal number (e.g. SIGTERM, SIGINT) of the first termination signal,
 *         or 0 if none has been received
 */
int get_quit_signal(void);

/**
 * @brief Reset all signal handlers installed by configure_signal_handler()
 *        back to their default dispositions (SIG_DFL)
 *
 * @return 0 on success, -1 on failure (errno set; error logged to stderr)
 *
 * Resets SIGINT, SIGQUIT, SIGTERM, SIGHUP, and SIGPIPE to SIG_DFL.
 */
int reset_signal_handlers_to_default(void);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_SIGNAL_HANDLER_H */
