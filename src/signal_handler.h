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
 * @brief Configure signal handlers for graceful termination
 * @note This function sets up signal handlers for termination signals
 *       (SIGINT, SIGTERM, SIGHUP, SIGQUIT) to allow the program to
 *       exit gracefully.
 */
void configure_signal_handlers(void);

/**
 * @brief Check if the quit flag is set
 * @return 1 if the quit flag is set, 0 otherwise
 */
int is_quit_flag_set(void);

#endif
