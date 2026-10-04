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

#include "cpu_count.h"
#include "file_io.h"
#include "path_util.h"
#include "process_finder.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Display usage information
 *
 * @param stream Output stream (stdout for normal help, stderr for errors)
 * @param cfg Pointer to configuration structure (used for program_name display)
 *
 * Prints the usage message with all available options and targets; deciding
 * on an exit status is left to the caller.
 */
static void print_usage(FILE *stream, const struct cpulimit_cfg *cfg) {
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
    fprintf(
        stream,
        "  -e FILE, --exe=FILE      executable name or path (matched against argv[0])\n");
    fprintf(
        stream,
        "  COMMAND [ARG]...         run the command and limit CPU usage (implies -z)\n");
}

/**
 * @brief Parse and validate the PID option from command-line argument
 *
 * @param pid_str String representation of the process ID
 * @param cfg Pointer to configuration structure to update
 * @return 0 on success, EXIT_FAILURE on error
 */
static int parse_pid_option(const char *pid_str, struct cpulimit_cfg *cfg) {
    char *endptr;
    long pid;
    pid_t pid_result;
    /*
     * Reject a leading whitespace before conversion. strtol() skips leading
     * whitespace, so "-p ' 5'" would otherwise be accepted as PID 5, which is
     * inconsistent with the strict trailing-character rejection.
     */
    if (pid_str == NULL || isspace((unsigned char)pid_str[0])) {
        /* above must not fall through to printing the pointer itself. */
        fprintf(stderr, "Error: invalid PID: %s\n\n",
                pid_str != NULL ? pid_str : "(null)");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    errno = 0;
    pid = strtol(pid_str, &endptr, 10);
    /*
     * Validate conversion: check for errors, empty strings, and trailing
     * characters. PID 0 and negative values are invalid.
     */
    if (errno != 0 || endptr == pid_str || *endptr != '\0' || pid < 1) {
        fprintf(stderr, "Error: invalid PID: %s\n\n", pid_str);
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /*
     * PID 1 (init/systemd) is never a valid target on any platform. No kill
     * -- not even the kill(pid,0) existence probe used elsewhere -- may be
     * sent to it, because suspending or interfering with init would freeze
     * or crash the whole system. There is no container or OS exception, so
     * it is rejected at input exactly like an out-of-range PID.
     */
    if (pid == 1) {
        fprintf(stderr, "Error: PID 1 (init) is not allowed as a target\n\n");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /* Verify PID fits within pid_t range (catch overflow on 32-bit systems) */
    pid_result = long_to_pid_t(pid);
    if (pid_result < 0) {
        fprintf(stderr, "Error: PID out of range: %s\n\n", pid_str);
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    cfg->target_pid = pid_result;
    cfg->lazy_mode = 1;
    return 0;
}

/**
 * @brief Parse and validate the CPU limit percentage from command-line argument
 *
 * @param limit_str String representation of the CPU limit percentage
 * @param cfg Pointer to configuration structure to update
 * @param ncpu Number of CPU cores in the system
 * @return 0 on success, EXIT_FAILURE on error
 */
static int parse_limit_option(const char *limit_str, struct cpulimit_cfg *cfg,
                              int ncpu) {
    char *endptr;
    double percent_limit;
    double max_limit;
    /*
     * Reject a leading whitespace before conversion. strtod() skips leading
     * whitespace, so "-l ' 50'" would otherwise be accepted as 50%, which is
     * inconsistent with the strict trailing-character rejection.
     */
    if (limit_str == NULL || isspace((unsigned char)limit_str[0])) {
        /* above must not fall through to printing the pointer itself. */
        fprintf(stderr, "Error: invalid limit value: %s\n\n",
                limit_str != NULL ? limit_str : "(null)");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    errno = 0;
    percent_limit = strtod(limit_str, &endptr);
    /*
     * Compute the upper bound as double to avoid int overflow on systems
     * with large CPU counts (100 * ncpu overflows if ncpu > INT_MAX/100).
     */
    max_limit = (double)ncpu * 100.0;
    /*
     * Validate the conversion and value:
     * - No conversion errors
     * - String was not empty
     * - No trailing characters
     * - Not NaN
     * - Within valid range: (0, ncpu * 100]
     */
    if (errno != 0 || endptr == limit_str || *endptr != '\0' ||
        !(percent_limit > 0 && percent_limit <= max_limit)) {
        fprintf(stderr, "Error: invalid limit value: %s\n\n", limit_str);
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /* Store as CPU cores (0.0 to ncpu) for internal calculations */
    cfg->cpu_limit = percent_limit / 100.0;
    return 0;
}

/**
 * @brief Ensure exactly one target specification method is provided
 *
 * @param cfg Pointer to configuration structure to validate
 * @return 0 on success, EXIT_FAILURE on error
 */
static int validate_target_options(const struct cpulimit_cfg *cfg) {
    int pid_mode = cfg->target_pid > 0, exe_mode = cfg->exe_name != NULL,
        command_mode = cfg->command_mode;

    if (pid_mode + exe_mode + command_mode != 1) {
        fprintf(stderr,
                "Error: specify exactly one target: -p, -e, or COMMAND\n\n");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    return 0;
}

#if defined(__linux__)
/**
 * @brief Refuse a name target that leads nowhere but PID 1 (init)
 *
 * @param cfg Pointer to the configuration naming the target
 * @return 0 when the target may still lead somewhere, EXIT_FAILURE when -e
 *         named init and nothing else carries the name
 *
 * PID 1 is never a valid target on any platform, and that rejection must
 * happen here -- before the limiter starts -- not deep in
 * find_process_by_name() where it would run on every watch-loop scan. We read
 * init's argv[0] once from /proc/1/cmdline and compare it the same way
 * find_process_by_name() compares every process, through
 * process_name_matches_cmd().
 *
 * Sharing the name is enough to be refused -- but not enough to refuse: the
 * lookup skips PID 1 on every platform, so a name init wears can still have
 * ordinary processes behind it, and those are limitable targets like any
 * other. Only when nothing else wears it does the name have nowhere left to
 * go, and only then is a refusal worth more than the "cannot be found" line
 * the first attempt would print anyway. macOS and FreeBSD have no
 * /proc/1/cmdline, so the finder's silent exclusion is their only guard and
 * this check is compiled out there; they never refuse the name, and simply
 * find whatever else is wearing it.
 */
static int reject_init_name_target(const struct cpulimit_cfg *cfg) {
    char *cmdline;

    if (cfg->exe_name == NULL) {
        return 0;
    }
    cmdline = read_file_contents("/proc/1/cmdline");
    if (cmdline == NULL) {
        return 0;
    }
    /*
     * /proc/1/cmdline is argv[0]\0argv[1]\0...; cmdline points at argv[0] and
     * stops at the first NUL, which is all the comparison may read.
     */
    if (process_name_matches_cmd(cfg->exe_name, cmdline) &&
        !process_name_has_non_init_match(cfg->exe_name)) {
        fprintf(stderr,
                "Error: target name '%s' resolves to PID 1 (init), "
                "which is never a valid target\n\n",
                cfg->exe_name);
        print_usage(stderr, cfg);
        free(cmdline);
        return EXIT_FAILURE;
    }
    free(cmdline);
    return 0;
}
#endif

/**
 * @brief Parse command line arguments and populate configuration structure
 *
 * @param argc Number of command-line arguments (from main)
 * @param argv Array of command-line argument strings (from main)
 * @param cfg Pointer to configuration structure to be filled with parsed values
 * @return 0 on success, EXIT_FAILURE on error, -1 when help was requested
 */
int parse_arguments(int argc, char **argv, struct cpulimit_cfg *cfg) {
    int option_char, ncpu;
    int pid_option_seen = 0, exe_option_seen = 0, limit_option_seen = 0;
    const struct option long_options[] = {
        {"pid", required_argument, NULL, 'p'},
        {"exe", required_argument, NULL, 'e'},
        {"limit", required_argument, NULL, 'l'},
        {"verbose", no_argument, NULL, 'v'},
        {"lazy", no_argument, NULL, 'z'},
        {"include-children", no_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    /*
     * Reject invalid API usage defensively to avoid crashes when called
     * from tests or embedded integrations with malformed inputs.
     * Checked first, before any other processing.
     */
    if (cfg == NULL) {
        fprintf(stderr, "Error: parse_arguments: cfg is NULL\n");
        return EXIT_FAILURE;
    }
    if (argv == NULL) {
        fprintf(stderr, "Error: parse_arguments: argv is NULL\n");
        return EXIT_FAILURE;
    }
    if (argc <= 0) {
        fprintf(stderr, "Error: parse_arguments: argc is %d\n", argc);
        return EXIT_FAILURE;
    }
    if (argv[0] == NULL) {
        fprintf(stderr, "Error: parse_arguments: argv[0] is NULL\n");
        return EXIT_FAILURE;
    }

    ncpu = get_ncpu();

    memset(cfg, 0, sizeof(struct cpulimit_cfg));
    cfg->program_name = get_file_basename(argv[0]);
    cfg->cpu_limit =
        -1.0; /* Negative value indicates limit not yet specified */

    /*
     * Reset getopt() global state so parse_arguments() remains re-entrant
     * across multiple invocations in the same process (e.g. unit tests).
     */
#if defined(__APPLE__) || defined(__FreeBSD__)
    optreset = 1;
#endif
#if defined(__GLIBC__)
    /*
     * glibc requires optind = 0 to fully reinitialize getopt_long() state.
     * This preserves deterministic behavior across repeated calls in tests
     * and on older glibc versions.
     */
    optind = 0;
#else
    optind = 1;
#endif
    opterr = 0; /* Suppress getopt's built-in error messages */
    /*
     * Process all options using getopt_long.
     * Leading '+' stops parsing at first non-option (for COMMAND mode).
     */
    while (1) {
        option_char =
            getopt_long(argc, argv, "+:p:e:l:vzih", long_options, NULL);
        if (option_char == -1) {
            break;
        }
        switch (option_char) {
        case 'p': /* Process ID target */
            if (pid_option_seen) {
                fprintf(
                    stderr,
                    "Error: duplicate PID option; use -p/--pid only once\n\n");
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            pid_option_seen = 1;
            if (parse_pid_option(optarg, cfg) != 0) {
                return EXIT_FAILURE;
            }
            break;

        case 'e': /* Executable name target */
            if (exe_option_seen) {
                fprintf(
                    stderr,
                    "Error: duplicate executable option; use -e/--exe only once\n\n");
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            exe_option_seen = 1;
            if (optarg == NULL || *optarg == '\0') {
                fprintf(stderr, "Error: invalid executable name\n\n");
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            /*
             * Reject match names that can never select a process:
             * find_process_by_name() compares against the basename for
             * relative names and returns 0 immediately when that basename is
             * empty, so "bin/", "a/b/", "//" and "/tmp/" are all
             * structurally unmatchable. A bare "/" is the same case (its
             * basename is empty). Handing any of them on would mean thirty
             * seconds of "cannot be found, retrying..." in non-lazy mode and
             * a misleading "cannot be found" in lazy mode; reject them up
             * front with one clear error.
             */
            if (get_file_basename(optarg)[0] == '\0') {
                fprintf(stderr, "Error: invalid match name '%s'\n\n", optarg);
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            cfg->exe_name = optarg;
            break;

        case 'l': /* CPU percentage limit */
            if (limit_option_seen) {
                fprintf(
                    stderr,
                    "Error: duplicate limit option; use -l/--limit only once\n\n");
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            limit_option_seen = 1;
            if (parse_limit_option(optarg, cfg, ncpu) != 0) {
                return EXIT_FAILURE;
            }
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
            print_usage(stdout, cfg);
            return -1;

        case '?': /* Unknown option */
            if (optopt) {
                fprintf(stderr, "Error: invalid option '-%c'\n\n", optopt);
            } else {
                fprintf(stderr, "Error: invalid option '%s'\n\n",
                        argv[optind - 1]);
            }
            print_usage(stderr, cfg);
            return EXIT_FAILURE;

        case ':': /* Missing required argument for an option */
            if (optopt) {
                fprintf(stderr, "Error: option '-%c' requires an argument\n\n",
                        optopt);
            } else {
                fprintf(stderr, "Error: option '%s' requires an argument\n\n",
                        argv[optind - 1]);
            }
            print_usage(stderr, cfg);
            return EXIT_FAILURE;

        default:
            fprintf(stderr, "Unknown error\n\n");
            print_usage(stderr, cfg);
            return EXIT_FAILURE;
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
        /*
         * An empty command name ("cpulimit -l 50 ''") would otherwise fall
         * through to execvp(""), which fails with a confusing 126/127. Reject
         * it up front so the user gets a clear error and exit code 1.
         */
        if (argv[optind][0] == '\0') {
            fprintf(stderr, "Error: empty command name\n\n");
            print_usage(stderr, cfg);
            return EXIT_FAILURE;
        }
    }

    if (validate_target_options(cfg) != 0) {
        return EXIT_FAILURE;
    }

    if (cfg->cpu_limit < 0) {
        fprintf(stderr, "CPU limit (-l/--limit) is required\n\n");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }

    /*
     * Reject a name target that resolves to PID 1 (init) here, before the
     * limiter ever starts, rather than discovering it deep in the watch loop.
     */
#if defined(__linux__)
    if (reject_init_name_target(cfg) != 0) {
        return EXIT_FAILURE;
    }
#endif

    if (cfg->verbose) {
        printf("%d CPU%s detected\n", ncpu, ncpu > 1 ? "s" : "");
    }
    return 0;
}
