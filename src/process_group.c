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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "list.h"
#include "process_group.h"
#include "process_iterator.h"
#include "process_table.h"
#include "util.h"

pid_t find_process_by_pid(pid_t pid)
{
    return (kill(pid, 0) == 0) ? pid : -pid;
}

pid_t find_process_by_name(const char *process_name)
{
    /* pid of the target process */
    pid_t pid = -1;

    /* process iterator */
    struct process_iterator it;
    struct process *proc;
    struct process_filter filter;
    /**
     * Flag for full path comparison:
     * - True (1) if process_name is an absolute path (starts with '/').
     * - False (0) if process_name is a relative path (does not start with '/').
     *
     * This determines whether to compare full paths or just basenames.
     */
    int full_path_cmp = process_name[0] == '/';
    const char *process_cmp_name =
        full_path_cmp ? process_name : file_basename(process_name);
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process\n");
        exit(EXIT_FAILURE);
    }

    filter.pid = 0;
    filter.include_children = 0;
    init_process_iterator(&it, &filter);
    while (get_next_process(&it, proc) != -1)
    {
        const char *cmd_cmp_name =
            full_path_cmp ? proc->command : file_basename(proc->command);
        /* process found */
        if (strncmp(cmd_cmp_name, process_cmp_name, sizeof(proc->command)) == 0)
        {
            if (pid < 0 || is_child_of(pid, proc->pid))
            {
                pid = proc->pid;
            }
        }
    }
    free(proc);
    if (close_process_iterator(&it) != 0)
        exit(EXIT_FAILURE);

    return (pid > 0) ? find_process_by_pid(pid) : 0;
}

int init_process_group(struct process_group *pgroup, pid_t target_pid, int include_children)
{
    /* hashtable initialization */
    pgroup->proctable = (struct process_table *)malloc(sizeof(struct process_table));
    if (pgroup->proctable == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
    process_table_init(pgroup->proctable, 2048);
    pgroup->target_pid = target_pid;
    pgroup->include_children = include_children;
    pgroup->proclist = (struct list *)malloc(sizeof(struct list));
    if (pgroup->proclist == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process list\n");
        exit(EXIT_FAILURE);
    }
    init_list(pgroup->proclist, sizeof(pid_t));
    if (get_current_time(&pgroup->last_update) != 0)
    {
        exit(EXIT_FAILURE);
    }
    update_process_group(pgroup);
    return 0;
}

int close_process_group(struct process_group *pgroup)
{
    if (pgroup->proclist != NULL)
    {
        clear_list(pgroup->proclist);
        free(pgroup->proclist);
        pgroup->proclist = NULL;
    }

    if (pgroup->proctable != NULL)
    {
        process_table_destroy(pgroup->proctable);
        free(pgroup->proctable);
        pgroup->proctable = NULL;
    }

    return 0;
}

static struct process *process_dup(const struct process *proc)
{
    struct process *p = (struct process *)malloc(sizeof(struct process));
    if (p == NULL)
    {
        fprintf(stderr, "Memory allocation failed for duplicated process\n");
        exit(EXIT_FAILURE);
    }
    return (struct process *)memcpy(p, proc, sizeof(struct process));
}

/* parameter in range 0-1 */
#define ALPHA 0.08
#define MIN_DT 20

void update_process_group(struct process_group *pgroup)
{
    struct process_iterator it;
    struct process *tmp_process;
    struct process_filter filter;
    struct timespec now;
    double dt;
    if (get_current_time(&now) != 0)
    {
        exit(EXIT_FAILURE);
    }
    tmp_process = (struct process *)malloc(sizeof(struct process));
    if (tmp_process == NULL)
    {
        fprintf(stderr, "Memory allocation failed for tmp_process\n");
        exit(EXIT_FAILURE);
    }
    /* time elapsed from previous sample (in ms) */
    dt = timediff_in_ms(&now, &pgroup->last_update);
    filter.pid = pgroup->target_pid;
    filter.include_children = pgroup->include_children;
    init_process_iterator(&it, &filter);
    clear_list(pgroup->proclist);
    init_list(pgroup->proclist, sizeof(pid_t));

    while (get_next_process(&it, tmp_process) != -1)
    {
        struct process *p = process_table_find(pgroup->proctable, tmp_process);
        if (p == NULL)
        {
            /* process is new. add it */
            tmp_process->cpu_usage = -1;
            p = process_dup(tmp_process);
            process_table_add(pgroup->proctable, p);
            add_elem(pgroup->proclist, p);
        }
        else
        {
            double sample;
            add_elem(pgroup->proclist, p);
            if (dt < MIN_DT)
                continue;
            /* process exists. update CPU usage */
            sample = (tmp_process->cputime - p->cputime) / dt;
            sample = MIN(sample, 1.0 * get_ncpu());
            if (p->cpu_usage < 0)
            {
                /* initialization */
                p->cpu_usage = sample;
            }
            else
            {
                /* usage adjustment */
                p->cpu_usage = (1.0 - ALPHA) * p->cpu_usage + ALPHA * sample;
            }
            p->cputime = tmp_process->cputime;
        }
    }
    free(tmp_process);
    close_process_iterator(&it);
    if (dt < MIN_DT)
        return;
    pgroup->last_update = now;
}

double get_process_group_cpu_usage(const struct process_group *pgroup)
{
    const struct list_node *node;
    double cpu_usage = -1;
    for (node = pgroup->proclist->first; node != NULL; node = node->next)
    {
        const struct process *p = (struct process *)node->data;
        if (p->cpu_usage < 0)
        {
            continue;
        }
        if (cpu_usage < 0)
        {
            cpu_usage = 0;
        }
        cpu_usage += p->cpu_usage;
    }
    return cpu_usage;
}

int remove_process(struct process_group *pgroup, pid_t pid)
{
    return process_table_del(pgroup->proctable, &pid);
}
