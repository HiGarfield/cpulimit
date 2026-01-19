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

#ifndef __CLI_H
#define __CLI_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>

/**
 * @struct cpulimitcfg
 * @brief Configuration structure for CPU limit parameters
 */
struct cpulimitcfg {
    /** Name of the program executable */
    const char *program_name;
    /** Target process ID to limit */
    pid_t target_pid;
    /** Executable name to search for */
    const char *exe_name;
    /** CPU usage limit (0.0 to N_CPU) */
    double limit;
    /** Flag to include child processes in the limit */
    int include_children;
    /** Exit if target process is not found or stopped */
    int lazy_mode;
    /** Enable verbose output mode */
    int verbose;
    /** Flag indicating command execution mode */
    int command_mode;
    /** Arguments for command execution */
    char *const *command_args;
};

/**
 * @brief Parse command line arguments and populate configuration.
 * @param argc Argument count from main()
 * @param argv Argument vector from main()
 * @param cfg Pointer to the configuration structure to populate
 */
void parse_arguments(int argc, char *const *argv, struct cpulimitcfg *cfg);

#endif
