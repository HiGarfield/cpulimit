#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli.h"
#include "util.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

static void print_usage_and_exit(FILE *stream, const struct cpulimitcfg *cfg, int exit_code)
{
    fprintf(stream, "Usage: %s [OPTIONS...] TARGET\n", cfg->program_name);
    fprintf(stream, "   OPTIONS:\n");
    fprintf(stream, "      -l LIMIT, --limit=LIMIT   CPU percentage limit from 0 to %d (required)\n", 100 * get_ncpu());
    fprintf(stream, "      -v, --verbose             show control statistics\n");
    fprintf(stream, "      -z, --lazy                exit if the target process is missing or stopped\n");
    fprintf(stream, "      -i, --include-children    also limit the child processes\n");
    fprintf(stream, "      -h, --help                display the help message and exit\n");
    fprintf(stream, "   TARGET must be exactly one of these:\n");
    fprintf(stream, "      -p PID, --pid=PID         PID of the target process (implies -z)\n");
    fprintf(stream, "      -e FILE, --exe=FILE       name or path of the executable file\n");
    fprintf(stream, "      COMMAND [ARGS]            run the command and limit CPU usage (implies -z)\n");
    exit(exit_code);
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

    /* Parse command line options */
    while ((opt = getopt_long(argc, argv, "+p:e:l:vzih", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'p': /* Process ID */
            cfg->target_pid = (pid_t)strtol(optarg, NULL, 10);
            if (cfg->target_pid <= 1 || cfg->target_pid >= get_pid_max())
            {
                fprintf(stderr, "Invalid PID: %s\n", optarg);
                print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            }
            cfg->lazy_mode = 1;
            break;

        case 'e': /* Executable name */
            cfg->exe_name = optarg;
            break;

        case 'l': /* CPU limit */
            cfg->limit = strtod(optarg, NULL);
            if (cfg->limit < 0 || cfg->limit > 100 * n_cpu)
            {
                fprintf(stderr, "Limit must be 0-%d\n", 100 * n_cpu);
                print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
            }
            cfg->limit /= 100.0;
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
    if ((cfg->target_pid > 0) + (cfg->exe_name != NULL) + (!!cfg->command_mode) != 1)
    {
        fprintf(stderr, "Must specify exactly one target\n");
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }

    /* Validate CPU limit */
    if (cfg->limit <= 0)
    {
        fprintf(stderr, "CPU limit (-l) is required\n");
        print_usage_and_exit(stderr, cfg, EXIT_FAILURE);
    }
}
