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
 * @brief Initialize a process table with specified hash size
 * @param pt Pointer to the process table structure to initialize
 * @param hashsize Number of buckets to allocate in the hash table
 *
 * Allocates memory for the hash table bucket array and initializes all
 * buckets to NULL. The hash table uses separate chaining for collision
 * resolution. If memory allocation fails, the program exits with an error.
 *
 * @note The caller must call process_table_destroy() to free resources
 */
void process_table_init(struct process_table *pt, size_t hashsize) {
    if (pt == NULL) {
        return;
    }
    pt->hashsize = hashsize;
    /* Allocate bucket array; calloc initializes all pointers to NULL */
    if ((pt->table = (struct list **)calloc(pt->hashsize,
                                            sizeof(struct list *))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Compute hash bucket index for a given PID
 * @param pt Pointer to the process table
 * @param pid Process ID to hash
 * @return Bucket index in range [0, hashsize-1]
 *
 * Uses simple modulo hashing. The PID is cast to size_t to ensure
 * positive values before taking the modulo.
 */
static size_t pid_hash(const struct process_table *pt, pid_t pid) {
    return (size_t)pid % pt->hashsize;
}

/**
 * @brief Look up a process in the table by its PID
 * @param pt Pointer to the process table to search
 * @param pid Process ID to search for
 * @return Pointer to the process structure if found, NULL otherwise
 *
 * Performs O(1) average-case lookup by hashing the PID to determine the
 * bucket, then searching the linked list in that bucket. Returns NULL if
 * the process table is NULL, the bucket is empty, or the PID is not found.
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
    /* Search the linked list in this bucket, comparing PIDs */
    return (struct process *)locate_elem(
        pt->table[idx], &pid, offsetof(struct process, pid), sizeof(pid_t));
}

/**
 * @brief Insert a process into the hash table
 * @param pt Pointer to the process table
 * @param p Pointer to the process structure to insert
 *
 * Adds the process to the appropriate bucket based on its PID hash.
 * If the bucket doesn't exist, creates a new linked list for it.
 * Exits with an error if memory allocation fails.
 *
 * @note If a process with the same PID is already present in the table, the
 *  existing entry is left unchanged and the new process is not inserted
 *  (duplicate PIDs are ignored).
 */
void process_table_add(struct process_table *pt, struct process *p) {
    size_t idx;
    if (pt == NULL || p == NULL) {
        return;
    }
    idx = pid_hash(pt, p->pid);
    if (pt->table[idx] == NULL) {
        /* Bucket is empty; create new linked list for this bucket */
        if ((pt->table[idx] = (struct list *)malloc(sizeof(struct list))) ==
            NULL) {
            fprintf(stderr, "Memory allocation failed for the process list\n");
            exit(EXIT_FAILURE);
        }
        init_list(pt->table[idx]);
    }
    /* Verify process doesn't already exist before adding */
    if (locate_elem(pt->table[idx], &p->pid, offsetof(struct process, pid),
                    sizeof(pid_t)) == NULL) {
        if (add_elem(pt->table[idx], p) == NULL) {
            fprintf(stderr, "Failed to add process with PID %d to the table\n",
                    p->pid);
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * @brief Remove a process from the hash table by PID
 * @param pt Pointer to the process table
 * @param pid Process ID of the process to remove
 * @return 0 on successful deletion, 1 if process not found or table is NULL
 *
 * Locates the process by PID, removes its node from the linked list, and
 * frees the node. If removing the last node from a bucket, also frees the
 * bucket's linked list structure. The process data itself is freed by this
 * operation.
 */
int process_table_del(struct process_table *pt, pid_t pid) {
    struct list_node *node;
    size_t idx;
    if (pt == NULL) {
        return 1;
    }
    idx = pid_hash(pt, pid);
    if (pt->table[idx] == NULL) {
        return 1; /* Bucket is empty */
    }
    node = locate_node(pt->table[idx], &pid, offsetof(struct process, pid),
                       sizeof(pid_t));
    if (node == NULL) {
        return 1; /* Process not found in bucket */
    }
    /* Remove node and free its data */
    destroy_node(pt->table[idx], node);
    /* If bucket is now empty, free the list structure */
    if (is_empty_list(pt->table[idx])) {
        free(pt->table[idx]);
        pt->table[idx] = NULL;
    }
    return 0;
}

/**
 * @brief Destroy the hash table and free all associated memory
 * @param pt Pointer to the process table to destroy
 *
 * Iterates through all buckets, destroying each linked list and its
 * contents (including process data), then frees the bucket array itself.
 * After destruction, the table pointer is set to NULL.
 *
 * @note Safe to call with NULL pointer (does nothing)
 */
void process_table_destroy(struct process_table *pt) {
    size_t idx;
    if (pt == NULL) {
        return;
    }
    /* Free each bucket's linked list and its contents */
    for (idx = 0; idx < pt->hashsize; idx++) {
        if (pt->table[idx] != NULL) {
            destroy_list(pt->table[idx]);
            free(pt->table[idx]);
        }
    }
    /* Free the bucket array itself */
    free((void *)pt->table);
    pt->table = NULL;
}
