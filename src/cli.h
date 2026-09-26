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

#ifndef CPULIMIT_CLI_H
#define CPULIMIT_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * @struct cpulimit_cfg
 * @brief Configuration structure containing all runtime parameters for CPU
 *        limiting
 *
 * This structure stores the parsed command-line options and determines
 * the program's execution mode and behavior.
 */
struct cpulimit_cfg {
    /**
     * @brief Program name (basename of argv[0]) used in usage messages.
     */
    const char *program_name;

    /**
     * @brief Target process ID when using -p/--pid option.
     * 0 if not specified.
     */
    pid_t target_pid;

    /**
     * @brief Executable name/path when using -e/--exe option.
     * NULL if not specified.
     */
    const char *exe_name;

    /**
     * @brief CPU usage limit expressed in CPU cores (core equivalents), in
     * the range (0, N_CPU].
     */
    double cpu_limit;

    /**
     * @brief Non-zero to apply the limit to the target and all descendants.
     * Zero to limit only the target process.
     */
    int include_children;

    /**
     * @brief Non-zero to exit when the target terminates or is not found.
     * Zero to keep searching.
     */
    int lazy_mode;

    /**
     * @brief Non-zero to print CPU usage and control statistics.
     * Zero for silent operation.
     */
    int verbose;

    /**
     * @brief Non-zero to fork and execute command_args.
     * Zero to search for an existing process.
     */
    int command_mode;

    /**
     * @brief Array of command-line arguments to execute (NULL-terminated) in
     * command mode.
     */
    char **command_args;
};

/**
 * @brief Parse command line arguments and populate configuration structure
 *
 * @param argc Number of command-line arguments (from main)
 * @param argv Array of command-line argument strings (from main)
 * @param cfg Pointer to configuration structure to be filled with parsed values
 * @return 0 on success, EXIT_FAILURE on error, -1 when help was requested
 */
int parse_arguments(int argc, char **argv, struct cpulimit_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CLI_H */
