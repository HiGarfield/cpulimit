/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli.h"
#include "limit_process.h"
#include "limiter.h"
#include "list.h"
#include "process_group.h"
#include "process_iterator.h"
#include "signal_handler.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * Quit handler function.
 * Clear the current line on console (fix for ^C issue).
 */
static void quit_handler(void)
{
    if (is_quit_flag_set() && isatty(STDOUT_FILENO))
    {
        printf("\r");
        fflush(stdout);
    }
}

int main(int argc, char *argv[])
{
    /* Configuration struct */
    struct cpulimitcfg cfg;

    /* Register the quit handler to run at program exit */
    if (atexit(quit_handler) != 0)
    {
        fprintf(stderr, "Failed to register quit_handler\n");
        exit(EXIT_FAILURE);
    }

    /* Parse command line arguments */
    parse_arguments(argc, argv, &cfg);

    /* Set up signal handlers for SIGINT and SIGTERM */
    configure_signal_handlers();

    if (cfg.command_mode)
    {
        run_command_mode(&cfg);
    }
    else
    {
        run_pid_or_exe_mode(&cfg);
    }

    return 0;
}
