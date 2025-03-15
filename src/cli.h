
#ifndef __CLI_H
#define __CLI_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <sys/types.h>

/**
 * @struct cpulimitcfg
 * @brief Configuration structure for CPU limitation parameters
 */
struct cpulimitcfg
{
    /** Name of the program executable */
    const char *program_name;
    /** Target process ID to limit */
    pid_t target_pid;
    /** Executable name to search for */
    const char *exe_name;
    /** CPU usage limit (0.0-N_CPU) */
    double limit;
    /** Flag to include child processes */
    int include_children;
    /** Exit if target process not found */
    int lazy_mode;
    /** Verbose output flag */
    int verbose;
    /** Command execution mode flag */
    int command_mode;
    /** Arguments for command execution */
    char *const *command_args;
};

/**
 * @brief Parse command line arguments into configuration structure
 * @param argc Argument count from main()
 * @param argv Argument vector from main()
 * @param cfg Pointer to configuration structure to populate
 */
void parse_arguments(int argc, char *const *argv, struct cpulimitcfg *cfg);

#endif
