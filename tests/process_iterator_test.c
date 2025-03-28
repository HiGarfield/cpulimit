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
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        /* child is supposed to be killed by the parent :/ */
        while (1)
            sleep(5);
    }
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL)
    {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    filter.pid = getpid();
    filter.include_children = 1;
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0)
    {
        if (process->pid == getpid())
            assert(process->ppid == getppid());
        else if (process->pid == child)
            assert(process->ppid == getpid());
        else
            assert(0);
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 2);
    free(process);
    assert(close_process_iterator(&it) == 0);
    assert(kill(child, SIGKILL) == 0);
}

static void test_all_processes(void)
{
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count = 0;
    filter.pid = 0;
    filter.include_children = 0;
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

static void test_process_group_single(int include_children)
{
    struct process_group pgroup;
    int i;
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        /* child is supposed to be killed by the parent :/ */
        volatile int keep_running = 1;
        while (keep_running)
            ;
    }
    assert(init_process_group(&pgroup, child, include_children) == 0);
    for (i = 0; i < 100; i++)
    {
        struct list_node *node = NULL;
        int count = 0;
        update_process_group(&pgroup);
        assert(get_list_count(pgroup.proclist) == 1);
        for (node = pgroup.proclist->first; node != NULL; node = node->next)
        {
            const struct process *p = (const struct process *)node->data;
            assert(p->pid == child);
            assert(p->ppid == getpid());
            /* p->cpu_usage should be -1 or [0, NCPU] */
            assert((p->cpu_usage >= -1.00001 && p->cpu_usage <= -0.99999) ||
                   (p->cpu_usage >= 0 && p->cpu_usage <= 1.0 * get_ncpu()));
            count++;
        }
        assert(count == 1);
    }
    assert(close_process_group(&pgroup) == 0);
    assert(kill(child, SIGKILL) == 0);
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

int main(int argc __attribute__((unused)), char *argv[])
{
    /* ignore SIGINT and SIGTERM during tests*/
    struct sigaction sa;
    sa.sa_handler = ignore_signal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    command = argv[0];

    printf("Running test_single_process()...\n");
    test_single_process();
    printf("test_single_process() passed.\n");

    printf("Running test_multiple_process()...\n");
    test_multiple_process();
    printf("test_multiple_process() passed.\n");

    printf("Running test_all_processes()...\n");
    test_all_processes();
    printf("test_all_processes() passed.\n");

    printf("Running test_process_group_all()...\n");
    test_process_group_all();
    printf("test_process_group_all() passed.\n");

    printf("Running test_process_group_single()...\n");
    test_process_group_single(0);
    test_process_group_single(1);
    printf("test_process_group_single() passed.\n");

    printf("Running test_process_group_wrong_pid()...\n");
    test_process_group_wrong_pid();
    printf("test_process_group_wrong_pid() passed.\n");

    printf("Running test_process_name()...\n");
    test_process_name();
    printf("test_process_name() passed.\n");

    printf("Running test_find_process_by_pid()...\n");
    test_find_process_by_pid();
    printf("test_find_process_by_pid() passed.\n");

    printf("Running test_find_process_by_name()...\n");
    test_find_process_by_name();
    printf("test_find_process_by_name() passed.\n");

    printf("Running test_getppid_of()...\n");
    test_getppid_of();
    printf("test_getppid_of() passed.\n");

    printf("All tests passed.\n");

    return 0;
}
