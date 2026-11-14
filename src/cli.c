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
#include "process_iterator.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Print the usage message.
 *
 * @param stream Output stream.
 * @param cfg Configuration supplying the program name.
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
        "    -l LIMIT, --limit=LIMIT    CPU percentage limit, range (0, %ld] (required)\n",
        (long)ncpu * 100L);
    fprintf(
        stream,
        "    -v, --verbose              show control statistics and progress messages\n");
    fprintf(
        stream,
        "    -z, --lazy                 exit if the target process is not running\n");
    fprintf(
        stream,
        "    -i, --include-children     limit total CPU usage of target and descendants\n");
    fprintf(
        stream,
        "    -h, --help                 display this help message and exit\n\n");
    fprintf(stream, "TARGET must be exactly one of:\n");
    fprintf(
        stream,
        "    -p PID, --pid=PID          PID of the target process (implies -z)\n");
    fprintf(stream, "    -e FILE, --exe=FILE        executable name or path\n");
    fprintf(
        stream,
        "    COMMAND [ARG]...           run the command and limit CPU usage (implies -z)\n");
}

/**
 * @brief Parse and validate the -p argument.
 *
 * @param pid_str PID string.
 * @param cfg Configuration to update.
 * @return 0 on success, EXIT_FAILURE on error.
 */
static int parse_pid_option(const char *pid_str, struct cpulimit_cfg *cfg) {
    char *endptr;
    long pid;
    pid_t pid_result;
    /* Reject leading whitespace: strtol() would otherwise skip it. */
    if (pid_str == NULL || isspace((unsigned char)pid_str[0])) {
        fprintf(stderr, "Error: invalid PID: %s\n\n",
                pid_str != NULL ? pid_str : "(null)");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    errno = 0;
    pid = strtol(pid_str, &endptr, 10);
    if (errno != 0 || endptr == pid_str || *endptr != '\0' || pid < 1) {
        fprintf(stderr, "Error: invalid PID: %s\n\n", pid_str);
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /* Never target PID 1 on any platform. */
    /* Even the kill(pid, 0) probe is skipped for it. */
    if (pid == 1) {
        fprintf(stderr, "Error: PID 1 (init) is not allowed as a target\n\n");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /* Verify the PID fits in pid_t on 32-bit systems. */
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
 * @brief Parse and validate the -l argument.
 *
 * @param limit_str CPU percentage string.
 * @param cfg Configuration to update.
 * @param ncpu Number of online CPUs.
 * @return 0 on success, EXIT_FAILURE on error.
 */
static int parse_limit_option(const char *limit_str, struct cpulimit_cfg *cfg,
                              int ncpu) {
    char *endptr;
    double percent_limit;
    double max_limit;
    /* Reject leading whitespace: strtod() would otherwise skip it. */
    if (limit_str == NULL || isspace((unsigned char)limit_str[0])) {
        fprintf(stderr, "Error: invalid limit value: %s\n\n",
                limit_str != NULL ? limit_str : "(null)");
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /* Ignore ERANGE: strtod() also sets it for valid subnormal values. */
    percent_limit = strtod(limit_str, &endptr);
    max_limit = (double)ncpu * 100.0;
    if (endptr == limit_str || *endptr != '\0' ||
        !(percent_limit > 0 && percent_limit <= max_limit)) {
        fprintf(stderr, "Error: invalid limit value: %s\n\n", limit_str);
        print_usage(stderr, cfg);
        return EXIT_FAILURE;
    }
    /* Store the limit in core equivalents. */
    cfg->cpu_limit = percent_limit / 100.0;
    return 0;
}

/**
 * @brief Ensure exactly one target specification is provided.
 *
 * @param cfg Configuration to validate.
 * @return 0 on success, EXIT_FAILURE on error.
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

/**
 * @brief Tell whether the rejected option was spelled short.
 *
 * @param arg The argv element getopt_long() left optind pointing at.
 * @return Non-zero for a single-dash option, zero otherwise.
 *
 * @note optopt holds a short equivalent even for a typed long option.
 */
static int is_short_option_spelling(const char *arg) {
    if (arg[0] != '-' || arg[1] == '-') {
        return 0;
    }
    return arg[1] != '\0';
}

/**
 * @brief Name a rejected option without trusting optind.
 *
 * @param buf Destination buffer, always NUL-terminated.
 * @param buf_size Size of buf.
 * @param argc Number of arguments.
 * @param argv Argument vector.
 * @param long_index Index of the matched long option, or -1.
 * @param long_options Long option table.
 *
 * @note After an unknown option, optind differs between BSD libc, glibc and
 *       musl, so argv[optind - 1] is not always safe to read.
 */
static void format_rejected_option(char *buf, size_t buf_size, int argc,
                                   char **argv, int long_index,
                                   const struct option *long_options) {
    int index = optind - 1;
    const char *typed = NULL;

    if (index >= 0 && index < argc && argv[index] != NULL &&
        argv[index][0] == '-') {
        typed = argv[index];
    }
    if (typed != NULL && !is_short_option_spelling(typed)) {
        (void)snprintf(buf, buf_size, "%s", typed);
    } else if (long_index >= 0) {
        (void)snprintf(buf, buf_size, "--%s", long_options[long_index].name);
    } else if (optopt) {
        (void)snprintf(buf, buf_size, "-%c", optopt);
    } else {
        (void)snprintf(buf, buf_size, "?");
    }
}

#if defined(__linux__)
/**
 * @brief Refuse an -e name that leads nowhere but PID 1.
 *
 * @param cfg Configuration naming the target.
 * @return 0 when the name may still lead somewhere, EXIT_FAILURE otherwise.
 *
 * Reads init's argv[0] from /proc/1/cmdline and compares it through
 * process_name_matches_cmd(). A name init wears can still belong to ordinary
 * processes, so only a completed scan proving no other match may refuse it.
 *
 * @note Linux only: macOS and FreeBSD have no /proc/1/cmdline and rely on
 *       the finder's silent exclusion of PID 1.
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
    /* /proc/1/cmdline is NUL-separated; compare only its argv[0] field. */
    if (process_name_matches_cmd(cfg->exe_name, cmdline) &&
        process_name_has_non_init_match(cfg->exe_name) == 0) {
        fprintf(
            stderr,
            "Error: target name '%s' resolves to PID 1 (init), which is never a valid target\n\n",
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
int parse_arguments(int argc, char **argv, struct cpulimit_cfg *cfg) {
    int option_char, ncpu, i, long_index;
    int pid_option_seen = 0, exe_option_seen = 0, limit_option_seen = 0;
    char rejected_option[64];
    const struct option long_options[] = {
        {"pid", required_argument, NULL, 'p'},
        {"exe", required_argument, NULL, 'e'},
        {"limit", required_argument, NULL, 'l'},
        {"verbose", no_argument, NULL, 'v'},
        {"lazy", no_argument, NULL, 'z'},
        {"include-children", no_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

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
    for (i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            fprintf(stderr, "Error: parse_arguments: argv[%d] is NULL\n", i);
            return EXIT_FAILURE;
        }
    }

    ncpu = get_ncpu();

    memset(cfg, 0, sizeof(struct cpulimit_cfg));
    cfg->program_name = get_file_basename(argv[0]);
    cfg->cpu_limit =
        -1.0; /* Negative while the limit has not been specified yet. */

    /* Reset getopt() state so repeated parses in one process work. */
#if defined(__APPLE__) || defined(__FreeBSD__)
    optreset = 1;
#endif
#if defined(__linux__)
    /* Linux libcs clear clustered short-option state only when optind is 0. */
    optind = 0;
#else
    optind = 1;
#endif
    opterr = 0;
    /* Clear optopt so a stale value cannot name the wrong option. */
    optopt = 0;
    /* A leading '+' stops parsing at the first non-option for command mode. */
    while (1) {
        long_index = -1;
        option_char =
            getopt_long(argc, argv, "+:p:e:l:vzih", long_options, &long_index);
        if (option_char == -1) {
            break;
        }
        switch (option_char) {
        case 'p':
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

        case 'e':
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
            /* An empty basename can never match argv[0]. */
            if (get_file_basename(optarg)[0] == '\0') {
                fprintf(stderr, "Error: invalid match name '%s'\n\n", optarg);
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            /* Reject names longer than the iterator's command buffer. */
            if (strlen(optarg[0] == '/' ? optarg : get_file_basename(optarg)) >=
                (size_t)CMD_BUFF_SIZE) {
                fprintf(
                    stderr,
                    "Error: invalid match name: comparison name is too long (limit %d characters)\n\n",
                    (int)CMD_BUFF_SIZE - 1);
                print_usage(stderr, cfg);
                return EXIT_FAILURE;
            }
            cfg->exe_name = optarg;
            break;

        case 'l':
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

        case 'v':
            cfg->verbose = 1;
            break;

        case 'z':
            cfg->lazy_mode = 1;
            break;

        case 'i':
            cfg->include_children = 1;
            break;

        case 'h':
            print_usage(stdout, cfg);
            return -1;

        case '?':
            format_rejected_option(rejected_option, sizeof(rejected_option),
                                   argc, argv, long_index, long_options);
            fprintf(stderr, "Error: invalid option '%s'\n\n", rejected_option);
            print_usage(stderr, cfg);
            return EXIT_FAILURE;

        case ':':
            format_rejected_option(rejected_option, sizeof(rejected_option),
                                   argc, argv, long_index, long_options);
            fprintf(stderr, "Error: option '%s' requires an argument\n\n",
                    rejected_option);
            print_usage(stderr, cfg);
            return EXIT_FAILURE;

        default:
            fprintf(stderr, "Unknown error\n\n");
            print_usage(stderr, cfg);
            return EXIT_FAILURE;
        }
    }

    /* Remaining arguments form the command to execute. */
    if (optind < argc) {
        cfg->command_mode = 1;
        cfg->command_args = argv + optind;
        cfg->lazy_mode = 1;
        /* An empty command name yields a confusing 126/127. */
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

    /* Refuse an -e name that resolves only to PID 1. */
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
