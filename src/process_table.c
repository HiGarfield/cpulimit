#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <sys/types.h>
#include <stdio.h>
#include "process_table.h"

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
    free(pt->table);
    pt->table = NULL;
}
