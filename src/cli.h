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
 * @brief Parsed command-line options and the resulting run mode.
 */
struct cpulimit_cfg {
    /**
     * @brief Basename of argv[0], used in diagnostics.
     */
    const char *program_name;

    /**
     * @brief PID from -p, or 0 when unset.
     */
    pid_t target_pid;

    /**
     * @brief Executable name from -e, or NULL.
     */
    const char *exe_name;

    /**
     * @brief CPU limit in core equivalents, in (0, N_CPU].
     */
    double cpu_limit;

    /**
     * @brief Non-zero to include descendants.
     */
    int include_children;

    /**
     * @brief Non-zero to exit when the target is gone.
     */
    int lazy_mode;

    /**
     * @brief Non-zero to print statistics and diagnostics.
     */
    int verbose;

    /**
     * @brief Non-zero to execute command_args.
     */
    int command_mode;

    /**
     * @brief NULL-terminated command vector for command mode.
     */
    char **command_args;
};

/**
 * @brief Parse command-line arguments into a configuration structure.
 *
 * @param argc Number of arguments.
 * @param argv Argument vector.
 * @param cfg Structure to fill.
 * @return 0 on success, EXIT_FAILURE on error, -1 when help was requested.
 *
 * @note Rejects a NULL cfg or argv, a non-positive argc, and any NULL
 *       element in argv[0..argc-1].
 */
int parse_arguments(int argc, char **argv, struct cpulimit_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif
