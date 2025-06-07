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
#include "util.h"
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * Print usage information and exit.
 * @param stream Output stream (stdout or stderr)
 * @param cfg Configuration structure
 * @param exit_code Exit code (0 for success, non-zero for error)
 */
static void print_usage_and_exit(FILE *stream, const struct cpulimitcfg *cfg, int exit_code)
{
    fprintf(stream, "Usage: %s [OPTIONS...] TARGET\n", cfg->program_name);
    fprintf(stream, "   OPTIONS:\n");
    fprintf(stream, "      -l LIMIT, --limit=LIMIT   CPU percentage limit from 0 to %d (required)\n", 100 * get_ncpu());
    fprintf(stream, "      -v, --verbose             show control statistics\n");
    fprintf(stream, "      -z, --lazy                exit if the target process is missing or stopped\n");
    fprintf(stream, "      -i, --include-children    limit total CPU usage of target and descendants\n");
    fprintf(stream, "      -h, --help                display the help message and exit\n");
    fprintf(stream, "   TARGET must be exactly one of these:\n");
    fprintf(stream, "      -p PID, --pid=PID         PID of the target process (implies -z)\n");
    fprintf(stream, "      -e FILE, --exe=FILE       name or path of the executable file\n");
    fprintf(stream, "      COMMAND [ARGS]            run the command and limit CPU usage (implies -z)\n");
    exit(exit_code);
}

/**
 * Parse the PID option from the command line.
 * @param pid_str String containing the PID
 * @param cfg Configuration structure
 */
static void parse_pid_option(const char *pid_str, struct cpulimitcfg *cfg)
{
    char *endptr;
    long pid;
    errno = 0;
    pid = strtol(pid_str, &endptr, 10);
    if (errno != 0 || endptr == pid_str || *endptr != '\0' ||
        pid <= 1 || pid >= get_pid_max())
    {
        fprintf(stderr, "Invalid PID: %s\n", pid_str);
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
    cfg->target_pid = (pid_t)pid;
    cfg->lazy_mode = 1;
}

/**
 * Parse the CPU limit option from the command line.
 * @param limit_str String containing the CPU limit
 * @param cfg Configuration structure
 * @param n_cpu Number of CPUs
 */
static void parse_limit_option(const char *limit_str, struct cpulimitcfg *cfg,
                               int n_cpu)
{
    char *endptr;
    double percent_limit;
    errno = 0;
    percent_limit = strtod(limit_str, &endptr);
    if (errno != 0 || endptr == limit_str || *endptr != '\0' ||
        percent_limit < 0 || percent_limit > 100 * n_cpu)
    {
        fprintf(stderr, "Invalid limit value: %s\n", limit_str);
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
    cfg->limit = percent_limit / 100.0;
}

/**
 * Validate the target options to ensure exactly one target is specified.
 * @param cfg Configuration structure
 * @note This function checks that either a PID, executable name, or command is
 *       specified, but not more than one. If none or more than one is specified,
 *       it prints an error message and exits the program.
 */
static void validate_target_options(const struct cpulimitcfg *cfg)
{
    int pid_mode = cfg->target_pid > 0;
    int exe_mode = cfg->exe_name != NULL;
    int command_mode = !!cfg->command_mode;

    if (pid_mode + exe_mode + command_mode != 1)
    {
        fprintf(stderr, "Must specify exactly one target (PID, exe or command)\n");
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
}

void parse_arguments(int argc, char *const *argv, struct cpulimitcfg *cfg)
{
    int opt, n_cpu;
    const struct option long_options[] = {
        {"pid", required_argument, NULL, 'p'},
        {"exe", required_argument, NULL, 'e'},
        {"limit", required_argument, NULL, 'l'},
        {"verbose", no_argument, NULL, 'v'},
        {"lazy", no_argument, NULL, 'z'},
        {"include-children", no_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    /* Get number of CPUs */
    n_cpu = get_ncpu();

    /* Initialize configuration structure */
    memset(cfg, 0, sizeof(struct cpulimitcfg));
    cfg->program_name = file_basename(argv[0]);
    cfg->limit = -1.0; /* Default limit is -1 (unset) */

    /* Parse command line options */
    while ((opt = getopt_long(argc, argv, "+p:e:l:vzih", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'p': /* Process ID */
            parse_pid_option(optarg, cfg);
            break;

        case 'e': /* Executable name */
            cfg->exe_name = optarg;
            break;

        case 'l': /* CPU limit */
            parse_limit_option(optarg, cfg, n_cpu);
            break;

        case 'v': /* Verbose mode */
            cfg->verbose = 1;
            break;

        case 'z': /* Lazy mode */
            cfg->lazy_mode = 1;
            break;

        case 'i': /* Include children */
            cfg->include_children = 1;
            break;

        case 'h': /* Help */
            print_usage_and_exit(stdout, cfg, EXIT_SUCCESS);
            break;

        case '?': /* Invalid option */
            print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            break;

        default:
            print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
        }
    }

    /* Handle command mode */
    if (optind < argc)
    {
        cfg->command_mode = 1;
        cfg->command_args = argv + optind;
        cfg->lazy_mode = 1;
    }

    /* Validate target specification */
    validate_target_options(cfg);

    /* Limit was not set */
    if (cfg->limit < 0)
    {
        fprintf(stderr, "CPU limit (-l/--limit) is required\n");
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }

    /* Print number of CPUs if in verbose mode */
    if (cfg->verbose)
    {
        printf("%d CPU%s detected\n", n_cpu, n_cpu > 1 ? "s" : "");
    }
}
