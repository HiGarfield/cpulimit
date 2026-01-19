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

#include "cli.h"
#include "limiter.h"
#include "signal_handler.h"

int main(int argc, char *argv[]) {
    /* Configuration structure for CPU limiting */
    struct cpulimitcfg cfg;

    /* Parse command line arguments into configuration structure */
    parse_arguments(argc, argv, &cfg);

    /* Set up signal handlers for SIGINT and SIGTERM */
    configure_signal_handlers();

    /* Determine execution mode based on configuration */
    if (cfg.command_mode) {
        /* Run in command execution mode */
        run_command_mode(&cfg);
    } else {
        /* Run in PID or executable name search mode */
        run_pid_or_exe_mode(&cfg);
    }

    return 0;
}
