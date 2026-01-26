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

#undef NDEBUG

#include "../src/limit_process.h"
#include "../src/list.h"
#include "../src/process_group.h"
#include "../src/process_iterator.h"
#include "../src/signal_handler.h"
#include "../src/util.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Terminate a child process or process group and wait for it to exit
 * @param pid Process ID (positive) or negative process group ID
 * @param kill_signal Signal to send (SIGTERM or SIGKILL)
 * @note If pid > 0, treats pid as a single process. If pid < 0, treats
 *       -pid as a process group ID. Sends the given signal and waits up
 *       to 5 seconds. If SIGTERM times out, escalates to SIGKILL and
 *       waits an additional 5 seconds. If processes are not reaped after
 *       5 + 5 seconds, function exits.
 */
static void kill_and_wait(pid_t pid, int kill_signal) {
    struct timespec now, end_time;
    const struct timespec sleep_time = {0, 10000000L}; /* 10 ms */

    switch (kill_signal) {
    case SIGKILL:
    case SIGTERM:
        break;
    default:
        fprintf(stderr, "Invalid signal %d for kill_and_wait\n", kill_signal);
        return;
    }

    /* Initialize timeout: 5 seconds */
    get_current_time(&end_time);
    end_time.tv_sec += 5;

    kill(pid, kill_signal); /* Send initial signal */

    while (1) {
        pid_t wpid = waitpid(pid, NULL, WNOHANG);

        if (wpid > 0) { /* Process terminated */
            /* Single process: exit after cleanup */
            /* Process group: continue to next member */
            if (pid > 0) {
                break;
            }
        } else if (wpid == -1) { /* Waitpid error */
            if (errno != EINTR) {
                break; /* Non-interrupt error */
            }
        } else { /* wpid == 0: process still running */
            /* Check timeout */
            get_current_time(&now);
            if (now.tv_sec > end_time.tv_sec ||
                (now.tv_sec == end_time.tv_sec &&
                 now.tv_nsec >= end_time.tv_nsec)) {
                if (kill_signal == SIGTERM) {
                    /* SIGTERM timeout: escalate to SIGKILL */
                    kill(pid, SIGKILL);
                    kill_signal = SIGKILL;
                    /* Reset timeout for SIGKILL (5 seconds) */
                    get_current_time(&end_time);
                    end_time.tv_sec += 5;
                } else {
                    break; /* SIGKILL timeout: stop waiting */
                }
            }
            /* Short sleep to reduce CPU usage (10ms) */
            sleep_timespec(&sleep_time);
        }
    }

    /* Final cleanup: reap all remaining zombies */
    while (waitpid(pid, NULL, WNOHANG) > 0) {
        /* Keep reaping until none left */
    }
}

/**
 * @brief Test process iterator with a single process
 * @note Tests that the process iterator can retrieve the current process
 *       information correctly, both with and without child processes
 */
static void test_single_process(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Test without including children */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    count = 0;

    /* Initialize iterator and iterate through processes */
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        assert(process->pid == getpid());
        assert(process->ppid == getppid());
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 1);
    assert(close_process_iterator(&it) == 0);

    /* Test with including children */
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;
    count = 0;

    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        assert(process->pid == getpid());
        assert(process->ppid == getppid());
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 1);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

/**
 * @brief Test process iterator with multiple processes
 * @note Creates a child process and verifies that the iterator can
 *       retrieve both parent and child process information
 */
static void test_multiple_process(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count = 0;

    /* Create a child process for testing */
    pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process: sleep in a loop until killed */
        const struct timespec sleep_time = {5, 0L}; /* 5s */
        while (!is_quit_flag_set()) {
            sleep_timespec(&sleep_time);
        }
        exit(EXIT_SUCCESS);
    }

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Set up filter to include children */
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;

    /* Initialize iterator and verify both processes are found */
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        if (process->pid == getpid()) {
            assert(process->ppid == getppid());
        } else if (process->pid == child_pid) {
            assert(process->ppid == getpid());
        } else {
            assert(0);
        }
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 2);
    free(process);
    assert(close_process_iterator(&it) == 0);

    /* Clean up child process */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process iterator with all system processes
 * @note Verifies that the iterator can retrieve at least 10 processes
 *       and that the current process is correctly identified
 */
static void test_all_processes(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count = 0;

    /* Set up filter to get all processes */
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Initialize iterator and count processes */
    assert(init_process_iterator(&it, &filter) == 0);

    while (get_next_process(&it, process) == 0) {
        if (process->pid == getpid()) {
            assert(process->ppid == getppid());
            assert(process->cputime >= 0);
        }
        count++;
    }

    /* Verify we found at least 10 processes */
    assert(count >= 10);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

/**
 * @brief Test process group with all processes
 * @note Verifies that a process group initialized with PID 0 (all processes)
 *       contains at least 10 processes
 */
static void test_process_group_all(void) {
    struct process_group pgroup;
    const struct list_node *node = NULL;
    int count = 0;

    /* Initialize process group with all processes */
    assert(init_process_group(&pgroup, 0, 0) == 0);
    update_process_group(&pgroup);

    /* Count processes in the group */
    for (node = pgroup.proclist->first; node != NULL; node = node->next) {
        count++;
    }
    assert(count > 10);
    assert((size_t)count == get_list_count(pgroup.proclist));

    /* Update and verify again */
    update_process_group(&pgroup);
    assert(close_process_group(&pgroup) == 0);
}

/**
 * @brief Test process group with a single process
 * @param include_children Flag indicating whether to include child processes
 * @note Creates a child process and verifies that the process group correctly
 *       tracks it, with or without child process inclusion
 */
static void test_proc_group_single(int include_children) {
    struct process_group pgroup;
    int i;

    /* Create a child process for testing */
    pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process: busy loop until killed */
        while (!is_quit_flag_set()) {
            ;
        }
        exit(EXIT_SUCCESS);
    }

    /* Initialize process group with the child PID */
    assert(init_process_group(&pgroup, child_pid, include_children) == 0);

    /* Update process group 100 times and verify consistency */
    for (i = 0; i < 100; i++) {
        const struct list_node *node = NULL;
        int count = 0;

        update_process_group(&pgroup);
        assert(get_list_count(pgroup.proclist) == 1);

        for (node = pgroup.proclist->first; node != NULL; node = node->next) {
            const struct process *p = (const struct process *)node->data;
            assert(p->pid == child_pid);
            assert(p->ppid == getpid());
            /* p->cpu_usage should be -1 or [0, NCPU] */
            assert((p->cpu_usage >= -1.00001 && p->cpu_usage <= -0.99999) ||
                   (p->cpu_usage >= 0 && p->cpu_usage <= 1.0 * get_ncpu()));
            count++;
        }
        assert(count == 1);
    }
    assert(close_process_group(&pgroup) == 0);

    /* Clean up child process */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process group with a single process (both with and without
 * children)
 * @note Wrapper function to test process group with include_children set to
 *       0 and 1
 */
static void test_process_group_single(void) {
    /* Test without including children */
    test_proc_group_single(0);

    /* Test with including children */
    test_proc_group_single(1);
}

static char *command = NULL;

/**
 * @brief Test process name retrieval
 * @note Verifies that the process iterator can correctly retrieve the
 *       command name of the current process
 */
static void test_process_name(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    const char *proc_name1, *proc_name2;
    int cmp_result;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Set up filter to get current process with command reading */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 1;

    /* Get process information and verify command name */
    assert(init_process_iterator(&it, &filter) == 0);
    assert(get_next_process(&it, process) == 0);
    assert(process->pid == getpid());
    assert(process->ppid == getppid());

    /* Compare command names */
    proc_name1 = file_basename(command);
    proc_name2 = file_basename(process->command);
    cmp_result = strcmp(proc_name1, proc_name2);
    assert(cmp_result == 0);

    /* Verify no more processes */
    assert(get_next_process(&it, process) != 0);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

/**
 * @brief Test process group initialization with invalid PIDs
 * @note Verifies that process group initialization with invalid PIDs
 *       (-1 and INT_MAX) results in empty process lists
 */
static void test_process_group_wrong_pid(void) {
    struct process_group pgroup;

    /* Test with PID -1 */
    assert(init_process_group(&pgroup, -1, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);
    update_process_group(&pgroup);
    assert(get_list_count(pgroup.proclist) == 0);
    assert(close_process_group(&pgroup) == 0);

    /* Test with PID INT_MAX */
    assert(init_process_group(&pgroup, INT_MAX, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);
    update_process_group(&pgroup);
    assert(get_list_count(pgroup.proclist) == 0);
    assert(close_process_group(&pgroup) == 0);
}

/**
 * @brief Test find_process_by_pid function
 * @note Verifies that the current process can be found by its PID
 */
static void test_find_process_by_pid(void) {
    assert(find_process_by_pid(getpid()) == getpid());
}

/**
 * @brief Test find_process_by_name function
 * @note Tests various cases: correct process name, empty string,
 *       modified process names that should not match
 */
static void test_find_process_by_name(void) {
    char *wrong_name;
    size_t len;

    /* Allocate buffer for modified process names */
    if ((wrong_name = (char *)malloc(PATH_MAX + 1)) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /*
     * 'command' is the name of the current process (equivalent to argv[0]).
     * Verify that the find_process_by_name function can find the current
     * process (PID should match the return value of getpid()).
     */
    assert(find_process_by_name(command) == getpid());

    /*
     * Test Case 1: Pass an empty string to find_process_by_name.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, "");
    assert(find_process_by_name(wrong_name) == 0);

    /*
     * Test Case 2: Pass an incorrect process name by appending 'x'
     * to the current process's name.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, command); /* Copy the current process's name */
    strcat(wrong_name, "x");     /* Append 'x' to make it non-matching */
    assert(find_process_by_name(wrong_name) == 0);

    /*
     * Test Case 3: Pass a copy of the current process's name with
     * the last character removed.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, command); /* Copy the current process's name */
    len = strlen(wrong_name);
    wrong_name[len - 1] = '\0'; /* Remove the last character */
    assert(find_process_by_name(wrong_name) == 0);

    free(wrong_name);
}

/**
 * @brief Test getppid_of function
 * @note Verifies that getppid_of returns the correct parent PID for
 *       multiple processes, including the current process
 */
static void test_getppid_of(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;

    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Iterate through all processes and verify parent PID */
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        assert(getppid_of(process->pid) == process->ppid);
    }
    free(process);
    assert(close_process_iterator(&it) == 0);

    /* Verify current process's parent PID */
    assert(getppid_of(getpid()) == getppid());
}

/**
 * @brief Test limit_process function
 * @note Creates a process group with 4 processes and applies CPU limiting
 *       to verify that the CPU usage stays within the specified limit
 */
static void test_limit_process(void) {
    const double cpu_usage_limit = 0.5;
    pid_t child_pid;
    int sync_pipe[2], num_procs;
    num_procs = get_ncpu();
    num_procs = MAX(num_procs, 1);

    /* Create pipe for synchronization */
    assert(pipe(sync_pipe) == 0);

    /* Fork first child process */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid > 0) {
        /* Parent process: monitor CPU usage */
        pid_t limiter_pid;
        ssize_t bytes_read, ret;
        char ack;

        assert(close(sync_pipe[1]) == 0);

        /* Wait for num_procs acknowledgements (from num_procs processes) */
        for (bytes_read = 0; bytes_read < num_procs; bytes_read++) {
            ret = read(sync_pipe[0], &ack, 1);
            if (ret == -1 && errno == EINTR) {
                continue; /* Interrupted, retry read */
            }
            assert(ret == 1 && ack == 'A');
        }
        /* Now should read EOF */
        do {
            ret = read(sync_pipe[0], &ack, 1);
        } while (ret == -1 && errno == EINTR);
        assert(ret == 0);
        assert(close(sync_pipe[0]) == 0);
        assert(bytes_read == num_procs);
        /* Fork CPU limiter process */
        limiter_pid = fork();
        assert(limiter_pid >= 0);

        if (limiter_pid > 0) {
            /* Monitor process: track CPU usage */
            int i, count = 0;
            double cpu_usage = 0;
            struct process_group pgroup;
            const struct timespec sleep_time = {0, 500000000L};

            /* Initialize process group monitoring */
            assert(init_process_group(&pgroup, child_pid, 1) == 0);

            /* Monitor CPU usage over 60 iterations */
            for (i = 0; i < 60 && !is_quit_flag_set(); i++) {
                double temp_cpu_usage;
                sleep_timespec(&sleep_time);
                update_process_group(&pgroup);

                /* Verify all num_procs processes are being monitored */
                assert(get_list_count(pgroup.proclist) == (size_t)num_procs);

                temp_cpu_usage = get_process_group_cpu_usage(&pgroup);
                if (temp_cpu_usage > 0) {
                    cpu_usage += temp_cpu_usage;
                    count++;
                }
            }
            assert(close_process_group(&pgroup) == 0);
            assert(count > 0);

            /* Terminate limiter process first */
            kill_and_wait(limiter_pid, SIGKILL);

            /* Terminate entire process group */
            kill_and_wait(-child_pid, SIGKILL);

            /* Calculate and display average CPU usage */
            cpu_usage /= count;
            printf("CPU usage limit: %.3f, CPU usage: %.3f\n", cpu_usage_limit,
                   cpu_usage);

            /* Verify CPU usage */
            assert(cpu_usage <= get_ncpu());

            return;
        }
        /* limiter_pid == 0: CPU limiter process */
        limit_process(child_pid, cpu_usage_limit, 1, 0);
        exit(EXIT_SUCCESS);
    } else {
        /* child_pid == 0: Target process group */
        int i;

        /* Create new process group */
        setpgid(0, 0);

        assert(close(sync_pipe[0]) == 0);

        /* Fork (num_procs - 1) child processes */
        for (i = 1; i < num_procs; i++) {
            pid_t pid = fork();
            assert(pid >= 0);

            if (pid == 0) {
                /* Child process: do not create more children */
                break;
            }
        }

        /* Send acknowledgement and close pipe */
        assert(write(sync_pipe[1], "A", 1) == 1);
        assert(close(sync_pipe[1]) == 0);

        /* Keep processes running until terminated */
        while (!is_quit_flag_set()) {
            ;
        }
        exit(EXIT_SUCCESS);
    }
}

/** @def RUN_TEST(test_func)
 *  @brief Macro to run a test function and print its status
 *  @param test_func Name of the test function to run
 */
#define RUN_TEST(test_func)                                                    \
    do {                                                                       \
        printf("Running %s()...\n", #test_func);                               \
        test_func();                                                           \
        printf("%s() passed.\n", #test_func);                                  \
    } while (0)

/**
 * @brief Main test function
 * @param argc Argument count
 * @param argv Argument vector
 * @return 0 on success
 * @note Runs all test functions and prints their results. Ignores SIGINT
 *       and SIGTERM during testing to prevent interruption.
 */
int main(int argc, char *argv[]) {
    assert(argc >= 1);
    command = argv[0];

    configure_signal_handlers();
    printf("Starting tests...\n");
    RUN_TEST(test_single_process);
    RUN_TEST(test_multiple_process);
    RUN_TEST(test_all_processes);
    RUN_TEST(test_process_group_all);
    RUN_TEST(test_process_group_single);
    RUN_TEST(test_process_group_wrong_pid);
    RUN_TEST(test_process_name);
    RUN_TEST(test_find_process_by_pid);
    RUN_TEST(test_find_process_by_name);
    RUN_TEST(test_getppid_of);
    RUN_TEST(test_limit_process);
    printf("All tests passed.\n");

    return 0;
}
