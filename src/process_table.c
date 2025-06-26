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
#include "list.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

void process_table_init(struct process_table *pt, size_t hashsize)
{
    if (pt == NULL)
    {
        return;
    }
    pt->hashsize = hashsize;
    if ((pt->table = (struct list **)calloc(pt->hashsize, sizeof(struct list *))) == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
}

static size_t pid_hash(const struct process_table *pt, const void *procptr)
{
    return (size_t)(*((const pid_t *)procptr)) % pt->hashsize;
}

struct process *process_table_find(const struct process_table *pt,
                                   const void *procptr)
{
    size_t idx;
    if (pt == NULL || procptr == NULL)
    {
        return NULL;
    }
    idx = pid_hash(pt, procptr);
    if (pt->table[idx] == NULL)
    {
        return NULL;
    }
    return (struct process *)locate_elem(pt->table[idx], procptr);
}

void process_table_add(struct process_table *pt, struct process *p)
{
    size_t idx;
    if (pt == NULL || p == NULL)
    {
        return;
    }
    idx = pid_hash(pt, p);
    if (pt->table[idx] == NULL)
    {
        /* If the bucket is empty, create a new list and add the process */
        if ((pt->table[idx] = (struct list *)malloc(sizeof(struct list))) == NULL)
        {
            fprintf(stderr, "Memory allocation failed for the process list\n");
            exit(EXIT_FAILURE);
        }
        init_list(pt->table[idx], sizeof(pid_t));
        add_elem(pt->table[idx], p);
    }
    else
    {
        /* If the bucket already exists, check if the process exists */
        struct process *found_process =
            (struct process *)locate_elem(pt->table[idx], p);
        if (found_process != NULL)
        {
            /* Process already exists, update it */
            memcpy(found_process, p, sizeof(struct process));
        }
        else
        {
            /* Process does not exist, add it */
            add_elem(pt->table[idx], p);
        }
    }
}

int process_table_del(struct process_table *pt, const void *procptr)
{
    struct list_node *node;
    size_t idx;
    if (pt == NULL || procptr == NULL)
    {
        return 1;
    }
    idx = pid_hash(pt, procptr);
    if (pt->table[idx] == NULL)
    {
        return 1; /* nothing to delete */
    }
    node = locate_node(pt->table[idx], procptr);
    if (node == NULL)
    {
        return 1; /* nothing to delete */
    }
    destroy_node(pt->table[idx], node);
    /* Clean up empty bucket */
    if (is_empty_list(pt->table[idx]))
    {
        free(pt->table[idx]);
        pt->table[idx] = NULL;
    }
    return 0;
}

void process_table_destroy(struct process_table *pt)
{
    size_t idx;
    if (pt == NULL)
    {
        return;
    }
    for (idx = 0; idx < pt->hashsize; idx++)
    {
        if (pt->table[idx] != NULL)
        {
            destroy_list(pt->table[idx]);
            free(pt->table[idx]);
        }
    }
    free((void *)pt->table);
    pt->table = NULL;
}
