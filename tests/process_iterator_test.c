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
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>
#ifdef __linux__
#include <linux/limits.h>
#endif

#include "../src/process_iterator.h"
#include "../src/process_group.h"
#include "../src/util.h"

#ifndef __GNUC__
#define __attribute__(attr)
#endif

static void ignore_signal(int sig __attribute__((unused)))
{
}

static void test_single_process(void)
{
    struct process_iterator it;
    struct process process;
    struct process_filter filter;
    int count;
    /* don't iterate children */
    filter.pid = getpid();
    filter.include_children = 0;
    count = 0;
    init_process_iterator(&it, &filter);
    while (get_next_process(&it, &process) == 0)
    {
        assert(process.pid == getpid());
        assert(process.ppid == getppid());
        assert(process.cputime <= 100);
        count++;
    }
    assert(count == 1);
    close_process_iterator(&it);
    /* iterate children */
    filter.pid = getpid();
    filter.include_children = 0;
    count = 0;
    init_process_iterator(&it, &filter);
    while (get_next_process(&it, &process) == 0)
    {
        assert(process.pid == getpid());
        assert(process.ppid == getppid());
        assert(process.cputime <= 100);
        count++;
    }
    assert(count == 1);
    close_process_iterator(&it);
}

static void test_multiple_process(void)
{
    struct process_iterator it;
    struct process process;
    struct process_filter filter;
    int count = 0;
    pid_t child = fork();
    if (child == 0)
    {
        /* child is supposed to be killed by the parent :/ */
        while (1)
            sleep(5);
        exit(1);
    }
    filter.pid = getpid();
    filter.include_children = 1;
    init_process_iterator(&it, &filter);
    while (get_next_process(&it, &process) == 0)
    {
        if (process.pid == getpid())
            assert(process.ppid == getppid());
        else if (process.pid == child)
            assert(process.ppid == getpid());
        else
            assert(0);
        assert(process.cputime <= 100);
        count++;
    }
    assert(count == 2);
    close_process_iterator(&it);
    kill(child, SIGKILL);
}

static void test_all_processes(void)
{
    struct process_iterator it;
    struct process process;
    struct process_filter filter;
    int count = 0;
    filter.pid = 0;
    filter.include_children = 0;
    init_process_iterator(&it, &filter);

    while (get_next_process(&it, &process) == 0)
    {
        if (process.pid == getpid())
        {
            assert(process.ppid == getppid());
            assert(process.cputime <= 100);
        }
        count++;
    }
    assert(count >= 10);
    close_process_iterator(&it);
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
    update_process_group(&pgroup);
    assert(close_process_group(&pgroup) == 0);
}

static void test_process_group_single(int include_children)
{
    struct process_group pgroup;
    int i;
    pid_t child = fork();
    if (child == 0)
    {
        /* child is supposed to be killed by the parent :/ */
        volatile int unused_value = 0;
        increase_priority();
        while (1)
            (void)unused_value;
        exit(1);
    }
    assert(init_process_group(&pgroup, child, include_children) == 0);
    for (i = 0; i < 100; i++)
    {
        struct list_node *node = NULL;
        int count = 0;
        struct timespec interval;
        update_process_group(&pgroup);
        for (node = pgroup.proclist->first; node != NULL; node = node->next)
        {
            const struct process *p = (const struct process *)(node->data);
            assert(p->pid == child);
            assert(p->ppid == getpid());
            /* p->cpu_usage should be -1 or [0, 1] */
            assert((p->cpu_usage >= (-1.00001) && p->cpu_usage <= (-0.99999)) ||
                   (p->cpu_usage >= 0 && p->cpu_usage <= 1.05));
            count++;
        }
        assert(count == 1);
        interval.tv_sec = 0;
        interval.tv_nsec = 200000000;
        sleep_timespec(&interval);
    }
    assert(close_process_group(&pgroup) == 0);
    kill(child, SIGKILL);
}

char *command = NULL;

static void test_process_name(void)
{
    struct process_iterator it;
    struct process process;
    struct process_filter filter;
    static char command_basename[PATH_MAX];
    static char process_basename[PATH_MAX];
    filter.pid = getpid();
    filter.include_children = 0;
    init_process_iterator(&it, &filter);
    assert(get_next_process(&it, &process) == 0);
    assert(process.pid == getpid());
    assert(process.ppid == getppid());
    strncpy(command_basename, basename(command), sizeof(command_basename) - 1);
    command_basename[sizeof(command_basename) - 1] = '\0';
    strncpy(process_basename, basename(process.command), sizeof(process_basename) - 1);
    process_basename[sizeof(process_basename) - 1] = '\0';
    assert(strncmp(command_basename, process_basename, sizeof(command_basename)) == 0);
    assert(get_next_process(&it, &process) != 0);
    close_process_iterator(&it);
}

static void test_process_group_wrong_pid(void)
{
    struct process_group pgroup;
    assert(init_process_group(&pgroup, -1, 0) == 0);
    assert(pgroup.proclist->count == 0);
    update_process_group(&pgroup);
    assert(pgroup.proclist->count == 0);
    assert(init_process_group(&pgroup, 9999999, 0) == 0);
    assert(pgroup.proclist->count == 0);
    update_process_group(&pgroup);
    assert(pgroup.proclist->count == 0);
    assert(close_process_group(&pgroup) == 0);
}

static void test_find_process_by_pid(void)
{
    assert(find_process_by_pid(getpid()) == getpid());
}

static void test_find_process_by_name(void)
{
    assert(find_process_by_name(command) == getpid());
    assert(find_process_by_name("") == 0);
}

static void test_getppid_of(void)
{
    struct process_iterator it;
    struct process process;
    struct process_filter filter;
    filter.pid = 0;
    filter.include_children = 0;
    init_process_iterator(&it, &filter);
    while (get_next_process(&it, &process) == 0)
    {
        assert(getppid_of(process.pid) == process.ppid);
    }
    close_process_iterator(&it);
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
    increase_priority();
    test_single_process();
    test_multiple_process();
    test_all_processes();
    test_process_group_all();
    test_process_group_single(0);
    test_process_group_single(1);
    test_process_group_wrong_pid();
    test_process_name();
    test_find_process_by_pid();
    test_find_process_by_name();
    test_getppid_of();
    return 0;
}
