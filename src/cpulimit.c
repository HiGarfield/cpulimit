/**
 *
 * cpulimit - a CPU limiter for Linux
 *
 * Copyright (C) 2005-2012, by:  Angelo Marletta <angelo dot marletta at gmail dot com>
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
 **************************************************************
 *
 * This is a simple program to limit the cpu usage of a process
 * If you modify this code, send me a copy please
 *
 * Get the latest version at: http://github.com/opsengine/cpulimit
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "list.h"
#include "process_group.h"
#include "process_iterator.h"
#include "util.h"

#ifndef EPSILON
/* Define a very small value to avoid division by zero */
#define EPSILON 1e-12
#endif

/* Control time slot in microseconds */
/* Each slot is split into a working slice and a sleeping slice */
#define TIME_SLOT 100000

/* GLOBAL VARIABLES */

/* Define a global process group (family of processes) */
struct process_group pgroup;

/* PID of cpulimit */
pid_t cpulimit_pid;

/* Name of this program */
const char *program_name;

/* Number of CPUs available in the system */
int NCPU;

/* CONFIGURATION VARIABLES */

/* Verbose mode flag */
int verbose = 0;

/* Lazy mode flag (exit if no process is found) */
int lazy = 0;

/* Quit flag for handling SIGINT and SIGTERM signals */
volatile sig_atomic_t quit_flag = 0;

/**
 * Signal handler for SIGINT and SIGTERM signals.
 * Sets the quit_flag to 1 when a termination signal is received.
 *
 * @param sig Signal number (SIGINT or SIGTERM).
 */
static void sig_handler(int sig)
{
    /* Handle SIGINT and SIGTERM signals by setting quit_flag to 1 */
    switch (sig)
    {
    case SIGINT:
    case SIGTERM:
        quit_flag = 1;
        break;
    default:
        break;
    }
}

/**
 * Prints the usage information for the program and exit.
 *
 * @param stream The file stream to write the usage information to (e.g., stdout).
 * @param exit_code The exit code to return after printing usage.
 */
static void print_usage_and_exit(FILE *stream, int exit_code)
{
    /* Print the usage message along with available options */
    fprintf(stream, "Usage: %s [OPTIONS...] TARGET\n", program_name);
    fprintf(stream, "   OPTIONS\n");
    fprintf(stream, "      -l, --limit=N          percentage of cpu allowed from 0 to %d (required)\n", 100 * NCPU);
    fprintf(stream, "      -v, --verbose          show control statistics\n");
    fprintf(stream, "      -z, --lazy             exit if there is no target process, or if it dies\n");
    fprintf(stream, "      -i, --include-children limit also the children processes\n");
    fprintf(stream, "      -h, --help             display this help and exit\n");
    fprintf(stream, "   TARGET must be exactly one of these:\n");
    fprintf(stream, "      -p, --pid=N            pid of the process (implies -z)\n");
    fprintf(stream, "      -e, --exe=FILE         name of the executable program file or path name\n");
    fprintf(stream, "      COMMAND [ARGS]         run this command and limit it (implies -z)\n");
    fprintf(stream, "\nReport bugs to <marlonx80@hotmail.com>.\n");
    exit(exit_code);
}

/**
 * Dynamically calculates the time slot based on system load.
 * This allows the program to adapt to varying system conditions.
 *
 * @return The calculated dynamic time slot in microseconds.
 */
static double get_dynamic_time_slot(void)
{
    static double time_slot = TIME_SLOT;
    static const double MIN_TIME_SLOT = TIME_SLOT, /* Minimum allowed time slot */
        MAX_TIME_SLOT = TIME_SLOT * 5;             /* Maximum allowed time slot */
    static struct timespec last_update = {0, 0};
    struct timespec now;
    double load, new_time_slot;

    /* Skip updates if the last check was less than 1000 ms ago */
    if (get_current_time(&now) == 0 &&
        timediff_in_ms(&now, &last_update) < 1000.0)
    {
        return time_slot;
    }

    /* Get the system load average */
    if (getloadavg(&load, 1) != 1)
    {
        return time_slot;
    }

    last_update = now;

    /* Adjust the time slot based on system load and number of CPUs */
    new_time_slot = time_slot * load / NCPU / 0.3;
    new_time_slot = MAX(new_time_slot, MIN_TIME_SLOT);
    new_time_slot = MIN(new_time_slot, MAX_TIME_SLOT);

    /* Smoothly adjust the time slot using a moving average */
    time_slot = time_slot * 0.6 + new_time_slot * 0.4;

    return time_slot;
}

/**
 * Controls the CPU usage of a process (and optionally its children).
 * Limits the amount of time the process can run based on a given percentage.
 *
 * @param pid Process ID of the target process.
 * @param limit The CPU usage limit as a percentage (0.0 to 1.0).
 * @param include_children Whether to include child processes.
 */
static void limit_process(pid_t pid, double limit, int include_children)
{
    /* Slice of time in which the process is allowed to work */
    struct timespec twork;
    /* Slice of time in which the process is stopped */
    struct timespec tsleep;
    /* Generic list item for iterating over processes */
    struct list_node *node;
    /* Counter to help with printing status */
    int c = 0;

    /* The ratio of the time the process is allowed to work (range 0 to 1) */
    double workingrate = -1;

    /* Increase priority of the current process to reduce overhead */
    increase_priority();

    /* Initialize the process group (including children if needed) */
    init_process_group(&pgroup, pid, include_children);

    if (verbose)
        printf("Members in the process group owned by %ld: %d\n",
               (long)pgroup.target_pid, pgroup.proclist->count);

    /* Main loop to control the process until quit_flag is set */
    while (!quit_flag)
    {
        /* CPU usage of the controlled processes */
        /* 1 means that the processes are using 100% cpu */
        double pcpu = -1;
        double twork_total_nsec, tsleep_total_nsec;
        double time_slot;

        /* Update the process group, including checking for dead processes */
        update_process_group(&pgroup);

        /* Exit if no more processes are running */
        if (pgroup.proclist->count == 0)
        {
            if (verbose)
                printf("No more processes.\n");
            break;
        }

        /* Estimate CPU usage of all processes in the group */
        for (node = pgroup.proclist->first; node != NULL; node = node->next)
        {
            const struct process *proc = (const struct process *)(node->data);
            if (proc->cpu_usage < 0)
            {
                continue;
            }
            if (pcpu < 0)
                pcpu = 0;
            pcpu += proc->cpu_usage;
        }

        /* Adjust the work and sleep time slices based on CPU usage */
        if (pcpu < 0)
        {
            /* Initialize workingrate if it's the first cycle */
            pcpu = limit;
            workingrate = limit;
        }
        else
        {
            /* Adjust workingrate based on CPU usage and limit */
            workingrate = workingrate * limit / MAX(pcpu, EPSILON);
        }

        /* Clamp workingrate to the valid range (0, 1) */
        workingrate = MIN(workingrate, 1 - EPSILON);
        workingrate = MAX(workingrate, EPSILON);

        /* Get the dynamic time slot */
        time_slot = get_dynamic_time_slot();

        /* Calculate work and sleep times in nanoseconds */
        twork_total_nsec = time_slot * 1000 * workingrate;
        nsec2timespec(twork_total_nsec, &twork);

        tsleep_total_nsec = time_slot * 1000 - twork_total_nsec;
        nsec2timespec(tsleep_total_nsec, &tsleep);

        if (verbose)
        {
            /* Print CPU usage statistics every 10 cycles */
            if (c % 200 == 0)
                printf("\n%9s%16s%16s%14s\n",
                       "%CPU", "work quantum", "sleep quantum", "active rate");

            if (c % 10 == 0 && c > 0)
                printf("%8.2f%%%13.0f us%13.0f us%13.2f%%\n",
                       pcpu * 100, twork_total_nsec / 1000,
                       tsleep_total_nsec / 1000, workingrate * 100);
        }

        /* Resume processes in the group */
        node = pgroup.proclist->first;
        while (node != NULL)
        {
            struct list_node *next_node = node->next;
            const struct process *proc = (const struct process *)(node->data);
            if (kill(proc->pid, SIGCONT) != 0)
            {
                /* If the process is dead, remove it from the group */
                if (verbose)
                {
                    char errbuf[100];
                    sprintf(errbuf, "kill failed to send SIGCONT to process %ld",
                            (long)proc->pid);
                    perror(errbuf);
                }
                delete_node(pgroup.proclist, node);
                remove_process(&pgroup, proc->pid);
            }
            node = next_node;
        }

        /* Allow processes to run during the work slice */
        sleep_timespec(&twork);

        if (tsleep.tv_nsec > 0 || tsleep.tv_sec > 0)
        {
            /* Stop processes during the sleep slice if needed */
            node = pgroup.proclist->first;
            while (node != NULL)
            {
                struct list_node *next_node = node->next;
                const struct process *proc = (const struct process *)(node->data);
                if (kill(proc->pid, SIGSTOP) != 0)
                {
                    /* If the process is dead, remove it from the group */
                    if (verbose)
                    {
                        char errbuf[100];
                        sprintf(errbuf, "kill failed to send SIGSTOP to process %ld",
                                (long)proc->pid);
                        perror(errbuf);
                    }
                    delete_node(pgroup.proclist, node);
                    remove_process(&pgroup, proc->pid);
                }
                node = next_node;
            }
            /* Allow the processes to sleep during the sleep slice */
            sleep_timespec(&tsleep);
        }
        c = (c + 1) % 200;
    }

    /* If the quit_flag is set, resume all processes before exiting */
    if (quit_flag)
    {
        for (node = pgroup.proclist->first; node != NULL; node = node->next)
        {
            const struct process *p = (const struct process *)(node->data);
            kill(p->pid, SIGCONT);
        }
    }

    /* Clean up the process group */
    close_process_group(&pgroup);
}

/**
 * Handles the cleanup when a termination signal is received.
 * Clears the current line on the console if the quit flag is set.
 */
static void quit_handler(void)
{
    /* If quit_flag is set, clear the current line on console (fix for ^C issue) */
    if (quit_flag)
    {
        printf("\r");
    }
}

int main(int argc, char *argv[])
{
    /* Variables to store user-provided arguments */
    const char *exe = NULL;
    double perclimit = 0.0;
    int exe_ok = 0;
    int pid_ok = 0;
    int limit_ok = 0;
    pid_t pid = 0;
    int include_children = 0;
    int command_mode;

    /* For parsing command-line options */
    int next_option;
    int option_index = 0;

    /* Define valid short and long command-line options */
    const char *short_options = "+p:e:l:vzih";
    /* An array describing valid long options */
    const struct option long_options[] = {
        {"pid", required_argument, NULL, 'p'},
        {"exe", required_argument, NULL, 'e'},
        {"limit", required_argument, NULL, 'l'},
        {"verbose", no_argument, NULL, 'v'},
        {"lazy", no_argument, NULL, 'z'},
        {"include-children", no_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}};

    double limit;
    char *endptr;

    /* Set waiting time between process searches */
    struct timespec wait_time = {2, 0};

    /* Signal action struct for handling interrupts */
    struct sigaction sa;

    /* Register the quit handler to run at program exit */
    if (atexit(quit_handler) != 0)
    {
        fprintf(stderr, "Failed to register quit_handler\n");
        exit(EXIT_FAILURE);
    }

    /* Extract the program name and store it in program_base_name */
    program_name = file_basename(argv[0]);

    /* Get the current process ID */
    cpulimit_pid = getpid();

    /* Get the number of CPUs available */
    NCPU = get_ncpu();

    /* Parse the command-line options */
    do
    {
        next_option = getopt_long(argc, argv, short_options, long_options, &option_index);
        if (strchr("pel", next_option) != NULL && optarg[0] == '-')
        {
            fprintf(stderr, "%s: option '%c' requires an argument.\n",
                    argv[0], next_option);
            print_usage_and_exit(stderr, EXIT_FAILURE);
        }

        switch (next_option)
        {
        case 'p':
            /* Store the PID provided by the user */
            pid = (pid_t)strtol(optarg, &endptr, 10);
            pid_ok = endptr != optarg && *endptr == '\0';
            break;
        case 'e':
            /* Store the executable name provided by the user */
            exe = optarg;
            exe_ok = 1;
            break;
        case 'l':
            /* Store the CPU limit percentage provided by the user */
            perclimit = strtod(optarg, &endptr);
            limit_ok = endptr != optarg && *endptr == '\0';
            break;
        case 'v':
            /* Enable verbose mode */
            verbose = 1;
            break;
        case 'z':
            /* Enable lazy mode */
            lazy = 1;
            break;
        case 'i':
            /* Include child processes in the limit */
            include_children = 1;
            break;
        case 'h':
            /* Print usage information and exit */
            print_usage_and_exit(stdout, EXIT_SUCCESS);
            break;
        case '?':
            /* Print usage information on invalid option */
            print_usage_and_exit(stderr, EXIT_FAILURE);
            break;
        case -1:
            /* No more options to process */
            break;
        default:
            abort();
        }
    } while (next_option != -1);

    /* Validate provided PID */
    if (pid_ok && (pid <= 1 || pid >= get_pid_max()))
    {
        fprintf(stderr, "Error: Invalid value for argument PID\n");
        print_usage_and_exit(stderr, EXIT_FAILURE);
    }
    if (pid != 0)
    {
        /* Implicitly enable lazy mode if a PID is provided */
        lazy = 1;
    }

    /* Ensure that a CPU limit was specified */
    if (!limit_ok)
    {
        fprintf(stderr, "Error: You must specify a cpu limit percentage\n");
        print_usage_and_exit(stderr, EXIT_FAILURE);
    }

    /* Calculate the CPU limit as a fraction */
    limit = perclimit / 100.0;
    if (limit < 0 || limit > NCPU)
    {
        fprintf(stderr, "Error: limit must be in the range 0-%d00\n", NCPU);
        print_usage_and_exit(stderr, EXIT_FAILURE);
    }

    /* Determine if a command was provided */
    command_mode = optind < argc;

    /* Ensure exactly one target process (pid, executable, or command) is specified */
    if (exe_ok + pid_ok + command_mode != 1)
    {
        fprintf(stderr, "Error: You must specify exactly one target process by name, pid, or command line\n");
        print_usage_and_exit(stderr, EXIT_FAILURE);
    }

    /* Set up signal handlers for SIGINT and SIGTERM */
    sa.sa_handler = &sig_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Print number of CPUs if in verbose mode */
    if (verbose)
        printf("%d cpu detected\n", NCPU);

    /* Handle command mode (run a command and limit its CPU usage) */
    if (command_mode)
    {
        pid_t child;
        char *const *cmd_args = argv + optind;

        /* If verbose, print the command being executed */
        if (verbose)
        {
            int i;
            printf("Running command: '%s", cmd_args[0]);
            for (i = 1; i < argc - optind; i++)
            {
                printf(" %s", cmd_args[i]);
            }
            printf("'\n");
        }

        /* Fork a child process to run the command */
        child = fork();
        if (child < 0)
        {
            exit(EXIT_FAILURE);
        }
        else if (child == 0)
        {
            /* Execute the command in the child process */
            int ret = execvp(cmd_args[0], cmd_args);
            perror("Error"); /* Display error if execvp fails */
            exit(ret);
        }
        else
        {
            /* Parent process forks another limiter process to control CPU usage */
            pid_t limiter;
            limiter = fork();
            if (limiter < 0)
            {
                exit(EXIT_FAILURE);
            }
            else if (limiter > 0)
            {
                /* Wait for both child and limiter processes to complete */
                int status_process;
                int status_limiter;
                waitpid(child, &status_process, 0);
                waitpid(limiter, &status_limiter, 0);
                if (WIFEXITED(status_process))
                {
                    if (verbose)
                        printf("Process %ld terminated with exit status %d\n",
                               (long)child, WEXITSTATUS(status_process));
                    exit(WEXITSTATUS(status_process));
                }
                printf("Process %ld terminated abnormally\n", (long)child);
                exit(status_process);
            }
            else
            {
                /* Limiter process controls the CPU usage of the child process */
                if (verbose)
                    printf("Limiting process %ld\n", (long)child);
                limit_process(child, limit, include_children);
                exit(EXIT_SUCCESS);
            }
        }
    }

    /* Monitor and limit the target process specified by PID or executable name */
    while (!quit_flag)
    {
        pid_t ret = 0;
        if (pid_ok)
        {
            /* Search for the process by PID */
            ret = find_process_by_pid(pid);
            if (ret <= 0)
            {
                printf("No process found or you aren't allowed to control it\n");
            }
        }
        else
        {
            /* Search for the process by executable name */
            ret = find_process_by_name(exe);
            if (ret == 0)
            {
                printf("No process found\n");
            }
            else if (ret < 0)
            {
                printf("Process found but you aren't allowed to control it\n");
            }
            else
            {
                pid = ret;
            }
        }

        /* If a process is found, start limiting its CPU usage */
        if (ret > 0)
        {
            if (ret == cpulimit_pid)
            {
                printf("Target process %ld is cpulimit itself! Aborting because it makes no sense\n",
                       (long)ret);
                exit(EXIT_FAILURE);
            }
            printf("Process %ld found\n", (long)pid);
            limit_process(pid, limit, include_children);
        }

        /* Break the loop if lazy mode is enabled or quit flag is set */
        if (lazy || quit_flag)
            break;

        /* Wait for 2 seconds before the next process search */
        sleep_timespec(&wait_time);
    }

    return 0;
}
