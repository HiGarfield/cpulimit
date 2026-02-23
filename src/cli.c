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

#include "util.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @brief Display usage information and terminate the program
 * @param stream Output stream (stdout for normal help, stderr for errors)
 * @param cfg Pointer to configuration structure (used for program_name display)
 * @param exit_code Exit status code (0 for success, non-zero for error)
 *
 * Prints formatted usage message showing all available options and targets,
 * then exits the program with the specified exit code.
 */
static void print_usage_and_exit(FILE *stream, const struct cpulimitcfg *cfg,
                                 int exit_code) {
    int ncpu = get_ncpu();
    fprintf(stream, "Usage: %s OPTION... TARGET\n", cfg->program_name);
    fprintf(stream,
            "Limit the CPU usage of a process to a specified percentage.\n");
    fprintf(stream, "Example: %s -l 25 -e myapp\n\n", cfg->program_name);
    fprintf(stream, "Options:\n");
    fprintf(
        stream,
        "  -l LIMIT, --limit=LIMIT  CPU percentage limit, range (0, %ld] (required)\n",
        (long)ncpu * 100L);
    fprintf(stream, "  -v, --verbose            show control statistics\n");
    fprintf(
        stream,
        "  -z, --lazy               exit if the target process is not running\n");
    fprintf(
        stream,
        "  -i, --include-children   limit total CPU usage of target and descendants\n");
    fprintf(
        stream,
        "  -h, --help               display this help message and exit\n\n");
    fprintf(stream, "TARGET must be exactly one of:\n");
    fprintf(
        stream,
        "  -p PID, --pid=PID        PID of the target process (implies -z)\n");
    fprintf(stream,
            "  -e FILE, --exe=FILE      name or path of the executable\n");
    fprintf(
        stream,
        "  COMMAND [ARG]...         run the command and limit CPU usage (implies -z)\n");
    exit(exit_code);
}

/**
 * @brief Parse and validate the PID option from command-line argument
 * @param pid_str String representation of the process ID
 * @param cfg Pointer to configuration structure to update
 *
 * Converts the PID string to a numeric value using strtol, validates the range,
 * and stores it in cfg->target_pid. Automatically enables lazy mode since
 * monitoring a specific PID implies lazy behavior (exit when process
 * terminates).
 *
 * @note Exits the program with error message if PID is invalid or out of range
 */
static void parse_pid_option(const char *pid_str, struct cpulimitcfg *cfg) {
    char *endptr;
    long pid;
    pid_t pid_result;
    errno = 0;
    pid = strtol(pid_str, &endptr, 10);
    /*
     * Validate conversion: check for errors, empty strings, trailing
     * characters, and ensure PID is greater than 1 (PIDs 0 and 1 are reserved)
     */
    if (errno != 0 || endptr == pid_str || *endptr != '\0' || pid <= 1) {
        fprintf(stderr, "Error: invalid PID: %s\n\n", pid_str);
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
    /* Verify PID fits within pid_t range (catch overflow on 32-bit systems) */
    pid_result = long2pid_t(pid);
    if (pid_result < 0) {
        fprintf(stderr, "Error: PID out of range: %s\n\n", pid_str);
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
    cfg->target_pid = pid_result;
    /* PID targeting mode implies lazy behavior */
    cfg->lazy_mode = 1;
}

/**
 * @brief Test if a double-precision value is NaN (Not-a-Number)
 * @param value Value to test
 * @return 1 if value is NaN, 0 otherwise
 *
 * Uses compiler built-ins or the standard isnan() macro when available,
 * otherwise falls back to the IEEE 754 property that NaN != NaN, using
 * a volatile variable to prevent optimizations that could eliminate the
 * comparison.
 */
static int is_nan(double value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_isnan(value);
#elif defined(isnan) || defined(_ISOC99_SOURCE) ||                             \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) ||              \
    (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L)
    return isnan(value);
#else
    /* Fallback implementation for platforms without isnan support */
    volatile double temp = value;
    return temp != temp;
#endif
}

/**
 * @brief Parse and validate the CPU limit percentage from command-line argument
 * @param limit_str String representation of the CPU limit percentage
 * @param cfg Pointer to configuration structure to update
 * @param n_cpu Number of CPU cores in the system
 *
 * Converts limit string to a double-precision percentage value, validates
 * it is within the acceptable range (0, n_cpu*100], and stores the
 * normalized fraction (percentage/100) in cfg->limit.
 *
 * @note Exits the program with error message if limit is invalid or out of
 *       range
 */
static void parse_limit_option(const char *limit_str, struct cpulimitcfg *cfg,
                               int n_cpu) {
    char *endptr;
    double percent_limit;
    double max_limit;
    errno = 0;
    percent_limit = strtod(limit_str, &endptr);
    /*
     * Compute the upper bound as double to avoid int overflow on systems
     * with large CPU counts (100 * n_cpu overflows if n_cpu > INT_MAX/100).
     */
    max_limit = (double)n_cpu * 100.0;
    /*
     * Validate the conversion and value:
     * - No conversion errors
     * - String was not empty
     * - No trailing characters
     * - Not NaN
     * - Within valid range: (0, n_cpu * 100]
     */
    if (errno != 0 || endptr == limit_str || *endptr != '\0' ||
        is_nan(percent_limit) || percent_limit <= 0 ||
        percent_limit > max_limit) {
        fprintf(stderr, "Error: invalid limit value: %s\n\n", limit_str);
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
    /* Store as fraction (0.0 to n_cpu) for internal calculations */
    cfg->limit = percent_limit / 100.0;
}

/**
 * @brief Ensure exactly one target specification method is provided
 * @param cfg Pointer to configuration structure to validate
 *
 * Verifies that the user specified exactly one way to identify the target:
 * either -p (PID), -e (executable name), or COMMAND. Having zero or multiple
 * specifications is an error.
 *
 * @note Exits the program with error message if validation fails
 */
static void validate_target_options(const struct cpulimitcfg *cfg) {
    int pid_mode = cfg->target_pid > 0;
    int exe_mode = cfg->exe_name != NULL;
    int command_mode = cfg->command_mode;

    /* Verify exactly one target method is specified */
    if (pid_mode + exe_mode + command_mode != 1) {
        fprintf(stderr,
                "Error: specify exactly one target: -p, -e, or COMMAND\n\n");
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
}

/**
 * @brief Parse command line arguments and populate configuration structure
 * @param argc Number of command-line arguments (from main)
 * @param argv Array of command-line argument strings (from main)
 * @param cfg Pointer to configuration structure to be filled with parsed values
 *
 * This function processes all command-line options, validates the input,
 * and exits the program (via exit()) if any errors are encountered or if
 * help is requested. Upon successful return, cfg contains valid configuration.
 *
 * @note This function calls exit() and does not return on error or help request
 */
void parse_arguments(int argc, char *const *argv, struct cpulimitcfg *cfg) {
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

    /* Determine available CPU count for limit validation */
    n_cpu = get_ncpu();

    /* Initialize configuration with default values */
    memset(cfg, 0, sizeof(struct cpulimitcfg));
    cfg->program_name = file_basename(argv[0]);
    cfg->limit = -1.0; /* Negative value indicates limit not yet specified */

    /*
     * Reset getopt() global state so parse_arguments() remains re-entrant
     * across multiple invocations in the same process (e.g. unit tests).
     */
#if defined(__APPLE__) || defined(__FreeBSD__)
    optreset = 1;
#endif
    optind = 1;
    opterr = 0; /* Suppress getopt's built-in error messages */
    /*
     * Process all options using getopt_long.
     * Leading '+' stops parsing at first non-option (for COMMAND mode)
     */
    while ((opt = getopt_long(argc, argv, "+:p:e:l:vzih", long_options,
                              NULL)) != -1) {
        switch (opt) {
        case 'p': /* Process ID target */
            parse_pid_option(optarg, cfg);
            break;

        case 'e': /* Executable name target */
            if (optarg == NULL || *optarg == '\0') {
                fprintf(stderr, "Error: invalid executable name\n\n");
                print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            }
            cfg->exe_name = optarg;
            break;

        case 'l': /* CPU percentage limit */
            parse_limit_option(optarg, cfg, n_cpu);
            break;

        case 'v': /* Verbose statistics output */
            cfg->verbose = 1;
            break;

        case 'z': /* Lazy mode (exit when target stops) */
            cfg->lazy_mode = 1;
            break;

        case 'i': /* Include child processes in limiting */
            cfg->include_children = 1;
            break;

        case 'h': /* Display help and exit successfully */
            print_usage_and_exit(stdout, cfg, EXIT_SUCCESS);
            break;

        case '?': /* Unknown option */
            if (optopt) {
                fprintf(stderr, "Error: invalid option '-%c'\n\n", optopt);
            } else {
                fprintf(stderr, "Error: invalid option '%s'\n\n",
                        argv[optind - 1]);
            }
            print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            break;

        case ':': /* Missing required argument for an option */
            if (optopt) {
                fprintf(stderr, "Error: option '-%c' requires an argument\n\n",
                        optopt);
            } else {
                fprintf(stderr, "Error: option '%s' requires an argument\n\n",
                        argv[optind - 1]);
            }
            print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            break;

        default:
            fprintf(stderr, "Unknown error\n\n");
            print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            break;
        }
    }

    /*
     * Process any remaining non-option arguments as command to execute.
     * Command mode automatically enables lazy behavior.
     */
    if (optind < argc) {
        cfg->command_mode = 1;
        cfg->command_args = argv + optind;
        cfg->lazy_mode = 1;
    }

    /* Ensure exactly one target specification (PID, exe, or command) */
    validate_target_options(cfg);

    /* Verify CPU limit was specified (required parameter) */
    if (cfg->limit < 0) {
        fprintf(stderr, "CPU limit (-l/--limit) is required\n\n");
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }

    /* Display CPU count in verbose mode */
    if (cfg->verbose) {
        printf("%d CPU%s detected\n", n_cpu, n_cpu > 1 ? "s" : "");
    }
}
