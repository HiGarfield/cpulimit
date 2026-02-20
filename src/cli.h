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

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>

/**
 * @struct cpulimitcfg
 * @brief Configuration structure containing all runtime parameters for CPU
 *        limiting
 *
 * This structure stores the parsed command-line options and determines
 * the program's execution mode and behavior.
 */
struct cpulimitcfg {
    /**
     * Program name (basename of argv[0]) used in usage messages.
     */
    const char *program_name;

    /**
     * Target process ID when using -p/--pid option.
     * 0 if not specified.
     */
    pid_t target_pid;

    /**
     * Executable name/path when using -e/--exe option.
     * NULL if not specified.
     */
    const char *exe_name;

    /**
     * CPU usage limit as a fraction (percentage/100).
     * Range: (0, N_CPU].
     */
    double limit;

    /**
     * If 1, limit applies to target and all descendant processes.
     * If 0, target only.
     */
    int include_children;

    /**
     * If 1, exit when target process terminates or cannot be found.
     * If 0, keep searching.
     */
    int lazy_mode;

    /**
     * If 1, print CPU usage statistics and control information.
     * If 0, silent operation.
     */
    int verbose;

    /**
     * If 1, fork and execute command in command_args.
     * If 0, search for existing process.
     */
    int command_mode;

    /**
     * Array of command-line arguments to execute (NULL-terminated) in command
     * mode.
     */
    char *const *command_args;
};

/**
 * @brief Parse command line arguments and populate configuration structure
 * @param argc Number of command-line arguments (from main)
 * @param argv Array of command-line argument strings (from main)
 * @param cfg Pointer to configuration structure to be filled with parsed values
 *
 * This function processes all command-line options, validates the input,
 * and exits the program (via _exit()) if any errors are encountered or if
 * help is requested. Upon successful return, cfg contains valid configuration.
 *
 * @note This function calls _exit() and does not return on error or help
 *       request
 */
void parse_arguments(int argc, char *const *argv, struct cpulimitcfg *cfg);

#ifdef __cplusplus
}
#endif

#endif
