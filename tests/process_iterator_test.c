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

#undef NDEBUG

#include "../src/limit_process.h"
#include "../src/list.h"
#include "../src/process_group.h"
#include "../src/process_iterator.h"
#include "../src/util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void ignore_signal(int sig)
{
    (void)sig;
}

/**
 * Terminates a child process or process group and waits for it to exit.
 *
 * If pid > 0, treats pid as a single process.
 * If pid < 0, treats -pid as a process group ID.
 * Sends the given signal (SIGTERM or SIGKILL) and waits up to 5 seconds.
 * If SIGTERM times out, escalates to SIGKILL and waits an additional 5 seconds.
 * If processes are not reaped after 5 + 5 seconds, function exits.
 *
 * @param pid          The process ID or negative process group ID to terminate.
 * @param kill_signal  The signal to send to the process (SIGTERM or SIGKILL).
 *                     If SIGTERM is used, it will escalate to SIGKILL if the
 *                     process does not exit within 5 seconds.
 */
static void kill_and_wait(pid_t pid, int kill_signal)
{
    struct timespec now, end_time;
    const struct timespec sleep_time = {0, 10000000L}; /* 10 ms */

    switch (kill_signal)
    {
    case SIGKILL:
    case SIGTERM:
        break;
    default:
        fprintf(stderr,
                "Invalid signal %d for kill_and_wait\n", kill_signal);
        return;
    }

    /* Initialize timeout: 5 seconds */
    get_current_time(&end_time);
    end_time.tv_sec += 5;

    kill(pid, kill_signal); /* Send initial signal */

    while (1)
    {
        pid_t wpid = waitpid(pid, NULL, WNOHANG);

        if (wpid > 0)
        { /* Process terminated */
            /* Single process: exit after cleanup */
            /* Process group: continue to next member */
            if (pid > 0)
            {
                break;
            }
        }
        else if (wpid == -1)
        { /* Waitpid error */
            if (errno != EINTR)
            {
                break; /* Non-interrupt error */
            }
        }
        else
        { /* wpid == 0: process still running */
            /* Check timeout */
            get_current_time(&now);
            if (now.tv_sec > end_time.tv_sec ||
                (now.tv_sec == end_time.tv_sec &&
                 now.tv_nsec >= end_time.tv_nsec))
            {
                if (kill_signal == SIGTERM)
                {
                    /* SIGTERM timeout: escalate to SIGKILL */
                    kill(pid, SIGKILL);
                    kill_signal = SIGKILL;
                    /* Reset timeout for SIGKILL (5 seconds) */
                    get_current_time(&end_time);
                    end_time.tv_sec += 5;
                }
                else
                {
                    break; /* SIGKILL timeout: stop waiting */
                }
            }
            /* Short sleep to reduce CPU usage (10ms) */
            sleep_timespec(&sleep_time);
        }
    }

    /* Final cleanup: reap all remaining zombies */
    while (waitpid(pid, NULL, WNOHANG) > 0)
    {
        /* Keep reaping until none left */
    }
}

static void test_single_process(void)
{
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count;
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    /* don't iterate children */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    count = 0;
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0)
    {
        assert(process->pid == getpid());
        assert(process->ppid == getppid());
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 1);
    assert(close_process_iterator(&it) == 0);
    /* iterate children */
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;
    count = 0;
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0)
    {
        assert(process->pid == getpid());
        assert(process->ppid == getppid());
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 1);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

static void test_multiple_process(void)
{
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count = 0;
    pid_t child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0)
    {
        /* child is supposed to be killed by the parent :/ */
        const struct timespec sleep_time = {5, 0L}; /* 5s */
        volatile int keep_running = 1;
        while (keep_running)
        {
            sleep_timespec(&sleep_time);
        }
        exit(EXIT_SUCCESS);
    }
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0)
    {
        if (process->pid == getpid())
        {
            assert(process->ppid == getppid());
        }
        else if (process->pid == child_pid)
        {
            assert(process->ppid == getpid());
        }
        else
        {
            assert(0);
        }
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 2);
    free(process);
    assert(close_process_iterator(&it) == 0);
    kill_and_wait(child_pid, SIGKILL);
}

static void test_all_processes(void)
{
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count = 0;
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    assert(init_process_iterator(&it, &filter) == 0);

    while (get_next_process(&it, process) == 0)
    {
        if (process->pid == getpid())
        {
            assert(process->ppid == getppid());
            assert(process->cputime >= 0);
        }
        count++;
    }
    assert(count >= 10);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

static void test_process_group_all(void)
{
    struct process_group pgroup;
    struct list_node *node = NULL;
    int count = 0;
    assert(init_process_group(&pgroup, 0, 0) == 0);
    update_process_group(&pgroup);
    for (node = pgroup.proclist->first; node != NULL; node = node->next)
    {
        count++;
    }
    assert(count > 10);
    assert((size_t)count == get_list_count(pgroup.proclist));
    update_process_group(&pgroup);
    assert(close_process_group(&pgroup) == 0);
}

static void test_proc_group_single(int include_children)
{
    struct process_group pgroup;
    int i;
    pid_t child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0)
    {
        /* child is supposed to be killed by the parent :/ */
        volatile int keep_running = 1;
        while (keep_running)
        {
            ;
        }
        exit(EXIT_SUCCESS);
    }
    assert(init_process_group(&pgroup, child_pid, include_children) == 0);
    for (i = 0; i < 100; i++)
    {
        struct list_node *node = NULL;
        int count = 0;
        update_process_group(&pgroup);
        assert(get_list_count(pgroup.proclist) == 1);
        for (node = pgroup.proclist->first; node != NULL; node = node->next)
        {
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
    kill_and_wait(child_pid, SIGKILL);
}

static void test_process_group_single(void)
{
    /* Test without including children */
    test_proc_group_single(0);

    /* Test with including children */
    test_proc_group_single(1);
}

static char *command = NULL;

static void test_process_name(void)
{
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    const char *proc_name1, *proc_name2;
    int cmp_result;
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 1;
    assert(init_process_iterator(&it, &filter) == 0);
    assert(get_next_process(&it, process) == 0);
    assert(process->pid == getpid());
    assert(process->ppid == getppid());
    proc_name1 = file_basename(command);
    proc_name2 = file_basename(process->command);
    cmp_result = strncmp(proc_name1, proc_name2, sizeof(process->command));
    assert(cmp_result == 0);
    assert(get_next_process(&it, process) != 0);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

static void test_process_group_wrong_pid(void)
{
    struct process_group pgroup;

    assert(init_process_group(&pgroup, -1, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);
    update_process_group(&pgroup);
    assert(get_list_count(pgroup.proclist) == 0);
    assert(close_process_group(&pgroup) == 0);

    assert(init_process_group(&pgroup, INT_MAX, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);
    update_process_group(&pgroup);
    assert(get_list_count(pgroup.proclist) == 0);
    assert(close_process_group(&pgroup) == 0);
}

static void test_find_process_by_pid(void)
{
    assert(find_process_by_pid(getpid()) == getpid());
}

static void test_find_process_by_name(void)
{
    char *wrong_name;
    size_t len;

    if ((wrong_name = (char *)malloc(PATH_MAX + 1)) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /*
     * 'command' is the name of the current process (equivalent to argv[0]).
     * Verify that the find_process_by_name function can find the current process
     * (PID should match the return value of getpid()).
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

static void test_getppid_of(void)
{
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0)
    {
        assert(getppid_of(process->pid) == process->ppid);
    }
    free(process);
    assert(close_process_iterator(&it) == 0);
    assert(getppid_of(getpid()) == getppid());
}

static void test_limit_process(void)
{
    const double cpu_usage_limit = 0.5;
    pid_t child_pid;
    const char *sem_name = "/test_limit_process_sem";
    sem_t *sem = sem_open(sem_name, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0);
    assert(sem != SEM_FAILED);
    assert(sem_unlink(sem_name) == 0);
    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid > 0)
    {
        pid_t limiter_pid = fork();
        assert(limiter_pid >= 0);
        if (limiter_pid > 0)
        {
            int i, count = 0;
            double cpu_usage = 0;
            struct process_group pgroup;
            const struct timespec sleep_time = {0, 500000000L},
                                  retry_delay = {0, 10000000L};
            struct timespec end_time, current_time;

            assert(get_current_time(&end_time) == 0);
            end_time.tv_sec += 10; /* 10 seconds timeout */
            for (i = 0; i < 4;)
            {
                errno = 0;
                if (sem_trywait(sem) == 0)
                {
                    i++;
                    continue;
                }
                assert(errno == EAGAIN);
                assert(get_current_time(&current_time) == 0);
                if (current_time.tv_sec > end_time.tv_sec ||
                    (current_time.tv_sec == end_time.tv_sec &&
                     current_time.tv_nsec >= end_time.tv_nsec))
                {
                    fprintf(stderr, "Timeout waiting for child processes to start\n");
                    kill_and_wait(limiter_pid, SIGKILL);
                    kill_and_wait(-child_pid, SIGKILL);
                    assert(sem_close(sem) == 0);
                    exit(EXIT_FAILURE);
                }
                sleep_timespec(&retry_delay);
            }
            assert(sem_close(sem) == 0);

            /* Initialize process group monitoring */
            assert(init_process_group(&pgroup, child_pid, 1) == 0);

            /* Monitor CPU usage over 60 iterations */
            for (i = 0; i < 60; i++)
            {
                double temp_cpu_usage;
                sleep_timespec(&sleep_time);
                update_process_group(&pgroup);

                /* Verify all 4 processes are being monitored */
                assert(get_list_count(pgroup.proclist) == 4);

                temp_cpu_usage = get_process_group_cpu_usage(&pgroup);
                if (temp_cpu_usage > 0)
                {
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
            printf("CPU usage limit: %.3f, CPU usage: %.3f\n",
                   cpu_usage_limit, cpu_usage);

            /* Verify CPU usage */
            assert(cpu_usage <= get_ncpu());

            return;
        }
        /* limiter_pid == 0: CPU limiter process */
        assert(sem_close(sem) == 0);
        limit_process(child_pid, cpu_usage_limit, 1, 0);
        exit(EXIT_SUCCESS);
    }
    else
    {
        /* child_pid == 0: Target process group */
        volatile int keep_running = 1;

        /* Create new process group */
        setpgid(0, 0);

        /* Fork 4 child processes */
        assert(fork() >= 0);
        assert(fork() >= 0);

        assert(sem_post(sem) == 0);
        assert(sem_close(sem) == 0);

        /* Keep processes running until terminated */
        while (keep_running)
        {
            ;
        }
        exit(EXIT_SUCCESS);
    }
}

#define RUN_TEST(test_func)                      \
    do                                           \
    {                                            \
        printf("Running %s()...\n", #test_func); \
        test_func();                             \
        printf("%s() passed.\n", #test_func);    \
    } while (0)

int main(int argc, char *argv[])
{
    /* ignore SIGINT and SIGTERM during tests*/
    struct sigaction sa;
    sa.sa_handler = ignore_signal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    assert(argc >= 1);
    command = argv[0];

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
