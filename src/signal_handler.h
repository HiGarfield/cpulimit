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
 * @brief Install the unified termination-signal handler.
 *
 * Blocks all signals, clears the latch state, installs SA_RESTART handlers
 * for SIGINT, SIGQUIT, SIGTERM, SIGHUP and SIGPIPE, then restores the mask.
 *
 * @note Exits with an error when the mask or a registration fails.
 */
void configure_signal_handler(void);

/**
 * @brief Check whether a termination signal has been received.
 *
 * @return 1 when a termination signal was caught, 0 otherwise.
 */
int is_quit_flag_set(void);

/**
 * @brief Get the signal number that set the quit flag.
 *
 * @return Number of the first termination signal, or 0 if none was received.
 */
int get_quit_signal(void);

/**
 * @brief Reset the installed signal handlers to SIG_DFL.
 *
 * @return 0 on success, -1 on failure with errno set.
 */
int reset_signal_handlers_to_default(void);

#ifdef __cplusplus
}
#endif

#endif
