/*
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012  Angelo Marletta
 * <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_table.h"
#include "list.h"
#include "process_iterator.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

/**
 * @brief Initialize a process table with the given hash size
 * @param pt Pointer to the process table structure to initialize
 * @param hashsize Size of the hash table
 */
void process_table_init(struct process_table *pt, size_t hashsize) {
    if (pt == NULL) {
        return;
    }
    pt->hashsize = hashsize;
    /* Allocate memory for the hash table */
    if ((pt->table = (struct list **)calloc(pt->hashsize,
                                            sizeof(struct list *))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Calculate hash index for a process ID
 * @param pt Pointer to the process table structure
 * @param pid The process ID to hash
 * @return Hash index
 */
static size_t pid_hash(const struct process_table *pt, pid_t pid) {
    return (size_t)pid % pt->hashsize;
}

/**
 * @brief Find a process in the process table based on the PID
 * @param pt Pointer to the process table to search in
 * @param pid The process ID to search for
 * @return Pointer to the found process, or NULL if not found
 */
struct process *process_table_find(const struct process_table *pt, pid_t pid) {
    size_t idx;
    if (pt == NULL) {
        return NULL;
    }
    idx = pid_hash(pt, pid);
    if (pt->table[idx] == NULL) {
        return NULL;
    }
    return (struct process *)locate_elem(pt->table[idx], &pid);
}

/**
 * @brief Add a process to the process table
 * @param pt The process table to add the process to
 * @param p The process to add
 * @note This function should only be called when p does not exist in pt
 */
void process_table_add(struct process_table *pt, struct process *p) {
    size_t idx;
    if (pt == NULL || p == NULL) {
        return;
    }
    idx = pid_hash(pt, p->pid);
    if (pt->table[idx] == NULL) {
        /* If the bucket is empty, create a new one */
        if ((pt->table[idx] = (struct list *)malloc(sizeof(struct list))) ==
            NULL) {
            fprintf(stderr, "Memory allocation failed for the process list\n");
            exit(EXIT_FAILURE);
        }
        init_list(pt->table[idx], sizeof(pid_t));
    }
    add_elem(pt->table[idx], p);
}

/**
 * @brief Delete a process from the process table based on the PID
 * @param pt Pointer to the process table to delete the process from
 * @param pid The process ID of the process to delete
 * @return 0 if deletion is successful, 1 if process not found
 */
int process_table_del(struct process_table *pt, pid_t pid) {
    struct list_node *node;
    size_t idx;
    if (pt == NULL) {
        return 1;
    }
    idx = pid_hash(pt, pid);
    if (pt->table[idx] == NULL) {
        return 1; /* Nothing to delete */
    }
    node = locate_node(pt->table[idx], &pid);
    if (node == NULL) {
        return 1; /* Nothing to delete */
    }
    destroy_node(pt->table[idx], node);
    /* Clean up empty bucket */
    if (is_empty_list(pt->table[idx])) {
        free(pt->table[idx]);
        pt->table[idx] = NULL;
    }
    return 0;
}

/**
 * @brief Destroy the process table and free up the memory
 * @param pt Pointer to the process table to destroy
 */
void process_table_destroy(struct process_table *pt) {
    size_t idx;
    if (pt == NULL) {
        return;
    }
    for (idx = 0; idx < pt->hashsize; idx++) {
        if (pt->table[idx] != NULL) {
            destroy_list(pt->table[idx]);
            free(pt->table[idx]);
        }
    }
    free((void *)pt->table);
    pt->table = NULL;
}
