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

#ifndef __SIGNAL_HANDLER_H
#define __SIGNAL_HANDLER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/**
 * @brief Configure signal handler for graceful termination
 * @note This function sets up signal handler for termination signals
 *       to allow the program to exit gracefully.
 */
void configure_signal_handler(void);

/**
 * @brief Check if the quit flag is set
 * @return 1 if the quit flag is set, 0 otherwise
 */
int is_quit_flag_set(void);

/**
 * @brief Check if the program was terminated via terminal input
 * @return 1 if  termination was caused by terminal key (Ctrl+C, Ctrl+\),
 *         0 otherwise
 */
int is_terminated_by_tty(void);

#endif
