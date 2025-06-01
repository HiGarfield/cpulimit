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
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void ignore_signal(int sig)
{
    (void)sig;
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
        volatile int keep_running = 1;
        while (keep_running)
        {
            sleep(5);
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
    assert(kill(child_pid, SIGKILL) == 0);
    waitpid(child_pid, NULL, 0);
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
            ;
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
    assert(kill(child_pid, SIGKILL) == 0);
    waitpid(child_pid, NULL, 0);
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

    assert(init_process_group(&pgroup, PID_T_MAX, 0) == 0);
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

#define BUFFER_SIZE 30

static void test_limit_process(void)
{
    const double cpu_usage_limit = 0.5;
    const char *pipe_str = "test_limit_process";
    pid_t child_pid;
    int pipefd[2];
    assert(pipe(pipefd) == 0);
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
            char buf[BUFFER_SIZE];
            const struct timespec delay_time = {10, 0},
                                  sleep_time = {0, 500000000L};
            sleep_timespec(&delay_time);
            assert(init_process_group(&pgroup, child_pid, 1) == 0);
            for (i = 0; i < 40; i++)
            {
                double temp_cpu_usage;
                sleep_timespec(&sleep_time);
                update_process_group(&pgroup);
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
            close(pipefd[1]);
            assert(read(pipefd[0], buf, BUFFER_SIZE) ==
                   (ssize_t)(strlen(pipe_str) + 1));
            close(pipefd[0]);
            assert(strcmp(buf, pipe_str) == 0);
            assert(kill(-child_pid, SIGKILL) == 0);
            assert(kill(limiter_pid, SIGKILL) == 0);
            waitpid(limiter_pid, NULL, 0);
            while (waitpid(-child_pid, NULL, 0) > 0)
                ;
            cpu_usage /= count;
            printf("CPU usage limit: %.3f, CPU usage: %.3f\n",
                   cpu_usage_limit, cpu_usage);
            assert(cpu_usage <= get_ncpu());
            return;
        }
        /* limiter_pid == 0, limiter process */
        close(pipefd[0]);
        close(pipefd[1]);
        limit_process(child_pid, cpu_usage_limit, 1, 0);
        exit(EXIT_SUCCESS);
    }
    else
    {
        /* child_pid == 0, fork four child process to be limited */
        volatile int keep_running = 1;
        size_t write_len = strlen(pipe_str) + 1;
        setpgid(0, 0);
        close(pipefd[0]);
        assert(write(pipefd[1], pipe_str, write_len) == (ssize_t)write_len);
        close(pipefd[1]);
        assert(fork() >= 0);
        assert(fork() >= 0);
        while (keep_running)
            ;
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
