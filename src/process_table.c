#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "process_table.h"

void process_table_init(struct process_table *pt, int hashsize)
{
    pt->hashsize = hashsize;
    pt->table = (struct list **)malloc(sizeof(struct list *) * pt->hashsize);
    if (pt->table == NULL)
    {
        exit(1);
    }
    memset(pt->table, 0, sizeof(struct list *) * pt->hashsize);
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
    struct process p;
    p.pid = pid;
    return process_table_find(pt, &p);
}

void process_table_add(struct process_table *pt, struct process *p)
{
    int idx = proc_hash(pt, p);
    if (pt->table[idx] == NULL)
    {
        pt->table[idx] = (struct list *)malloc(sizeof(struct list));
        if (pt->table[idx] == NULL)
        {
            exit(-1);
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
    node = (struct list_node *)locate_node(pt->table[idx], p);
    if (node == NULL)
    {
        return 2; /* nothing to delete */
    }
    delete_node(pt->table[idx], node);
    return 0;
}

int process_table_del_pid(struct process_table *pt, pid_t pid)
{
    struct process p;
    p.pid = pid;
    return process_table_del(pt, &p);
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
