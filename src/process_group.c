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

#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include "process_iterator.h"
#include "process_group.h"
#include "list.h"
#include "util.h"
#include "process_table.h"

/* look for a process by pid
search_pid   : pid of the wanted process
return:  pid of the found process, if successful
         negative pid, if the process does not exist or if the signal fails */
pid_t find_process_by_pid(pid_t pid)
{
    return (kill(pid, 0) == 0) ? pid : -pid;
}

/* look for a process with a given name
process: the name of the wanted process. it can be an absolute path name to the executable file
        or just the file name
return:  pid of the found process, if it is found
        0, if it's not found
        negative pid, if it is found but it's not possible to control it */
pid_t find_process_by_name(char *process_name)
{
    /* pid of the target process */
    pid_t pid = -1;

    /* process iterator */
    struct process_iterator it;
    struct process proc;
    struct process_filter filter;
    const char *process_basename = basename(process_name);
    filter.pid = 0;
    filter.include_children = 0;
    init_process_iterator(&it, &filter);
    while (get_next_process(&it, &proc) != -1)
    {
        /* process found */
        if (strcmp(basename(proc.command), process_basename) == 0)
        {
            if (pid < 0)
            {
                pid = proc.pid;
            }
            else if (is_child_of(pid, proc.pid))
            {
                pid = proc.pid;
            }
            else if (is_child_of(proc.pid, pid))
            {
            }
            else
            {
                pid = MIN(proc.pid, pid);
            }
        }
    }
    if (close_process_iterator(&it) != 0)
        exit(1);
    if (pid > 0)
    {
        /* the process was found */
        return find_process_by_pid(pid);
    }
    else
    {
        /* process not found */
        return 0;
    }
}

int init_process_group(struct process_group *pgroup, pid_t target_pid, int include_children)
{
    /* hashtable initialization */
    pgroup->proctable = (struct process_table *)malloc(sizeof(struct process_table));
    if (pgroup->proctable == NULL)
    {
        exit(-1);
    }
    process_table_init(pgroup->proctable, 2048);
    pgroup->target_pid = target_pid;
    pgroup->include_children = include_children;
    pgroup->proclist = (struct list *)malloc(sizeof(struct list));
    if (pgroup->proclist == NULL)
    {
        exit(-1);
    }
    init_list(pgroup->proclist, sizeof(pid_t));
    if (get_time(&pgroup->last_update))
    {
        exit(-1);
    }
    update_process_group(pgroup);
    return 0;
}

int close_process_group(struct process_group *pgroup)
{
    clear_list(pgroup->proclist);
    free(pgroup->proclist);
    pgroup->proclist = NULL;
    process_table_destroy(pgroup->proctable);
    free(pgroup->proctable);
    pgroup->proctable = NULL;
    return 0;
}

static struct process *process_dup(struct process *proc)
{
    struct process *p = (struct process *)malloc(sizeof(struct process));
    if (p == NULL)
    {
        exit(-1);
    }
    return (struct process *)memcpy(p, proc, sizeof(struct process));
}

/* parameter in range 0-1 */
#define ALPHA 0.08
#define MIN_DT 20

void update_process_group(struct process_group *pgroup)
{
    struct process_iterator it;
    struct process tmp_process, *p;
    struct process_filter filter;
    struct timespec now;
    double dt;
    if (get_time(&now))
    {
        exit(1);
    }
    /* time elapsed from previous sample (in ms) */
    dt = timediff_in_ms(&now, &pgroup->last_update);
    filter.pid = pgroup->target_pid;
    filter.include_children = pgroup->include_children;
    init_process_iterator(&it, &filter);
    clear_list(pgroup->proclist);
    init_list(pgroup->proclist, sizeof(pid_t));

    while (get_next_process(&it, &tmp_process) != -1)
    {
        p = process_table_find(pgroup->proctable, &tmp_process);
        if (p == NULL)
        {
            /* process is new. add it */
            tmp_process.cpu_usage = -1;
            p = process_dup(&tmp_process);
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
            sample = (tmp_process.cputime - p->cputime) / dt;
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
            p->cputime = tmp_process.cputime;
        }
    }
    close_process_iterator(&it);
    if (dt < MIN_DT)
        return;
    pgroup->last_update = now;
}

int remove_process(struct process_group *pgroup, pid_t pid)
{
    return process_table_del_pid(pgroup->proctable, pid);
}
