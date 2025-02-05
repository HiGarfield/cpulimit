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

#include "process_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

void process_table_init(struct process_table *pt, int hashsize)
{
    pt->hashsize = hashsize;
    pt->table = (struct list **)calloc((size_t)pt->hashsize, sizeof(struct list *));
    if (pt->table == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
}

static int proc_hash(const struct process_table *pt, const struct process *p)
{
    return p->pid % pt->hashsize;
}

struct process *process_table_find(const struct process_table *pt, const struct process *p)
{
    int idx = proc_hash(pt, p);
    if (pt->table[idx] == NULL)
    {
        return NULL;
    }
    return (struct process *)locate_elem(pt->table[idx], p);
}

struct process *process_table_find_pid(const struct process_table *pt, pid_t pid)
{
    struct process *p, *res;
    p = (struct process *)malloc(sizeof(struct process));
    if (p == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process\n");
        exit(EXIT_FAILURE);
    }
    p->pid = pid;
    res = process_table_find(pt, p);
    free(p);
    return res;
}

void process_table_add(struct process_table *pt, struct process *p)
{
    int idx = proc_hash(pt, p);
    if (pt->table[idx] == NULL)
    {
        pt->table[idx] = (struct list *)malloc(sizeof(struct list));
        if (pt->table[idx] == NULL)
        {
            fprintf(stderr, "Memory allocation failed for the process list\n");
            exit(EXIT_FAILURE);
        }
        init_list(pt->table[idx], sizeof(pid_t));
    }
    add_elem(pt->table[idx], p);
}

int process_table_del(struct process_table *pt, const struct process *p)
{
    struct list_node *node;
    int idx = proc_hash(pt, p);
    if (pt->table[idx] == NULL)
    {
        return 1; /* nothing to delete */
    }
    node = locate_node(pt->table[idx], p);
    if (node == NULL)
    {
        return 1; /* nothing to delete */
    }
    delete_node(pt->table[idx], node);
    return 0;
}

int process_table_del_pid(struct process_table *pt, pid_t pid)
{
    struct process *p;
    int ret;
    p = (struct process *)malloc(sizeof(struct process));
    if (p == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process\n");
        exit(EXIT_FAILURE);
    }
    p->pid = pid;
    ret = process_table_del(pt, p);
    free(p);
    return ret;
}

void process_table_destroy(struct process_table *pt)
{
    int i;
    for (i = 0; i < pt->hashsize; i++)
    {
        if (pt->table[i] != NULL)
        {
            destroy_list(pt->table[i]);
            free(pt->table[i]);
        }
    }
    free((void *)pt->table);
    pt->table = NULL;
}
