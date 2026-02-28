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
 * @param proc_table Pointer to the process table structure to initialize
 * @param hash_size Number of buckets to allocate in the hash table
 *
 * Allocates memory for the hash table bucket array and initializes all
 * buckets to NULL. The hash table uses separate chaining for collision
 * resolution. If memory allocation fails, the program exits with an error.
 *
 * @note The caller must call ptbl_destroy() to free resources
 */
void ptbl_init(struct process_table *proc_table, size_t hash_size) {
    if (proc_table == NULL) {
        return;
    }
    if (hash_size == 0) {
        /*
         * Avoid zero-sized tables because pid_hash() uses modulo hash_size.
         * A single bucket still provides valid behavior for callers that
         * accidentally request size 0.
         */
        hash_size = 1;
    }
    proc_table->hash_size = hash_size;
    /* Allocate bucket array; calloc initializes all pointers to NULL */
    if ((proc_table->buckets = (struct list **)calloc(
             proc_table->hash_size, sizeof(struct list *))) == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Compute hash bucket index for a given PID
 * @param proc_table Pointer to the process table
 * @param pid Process ID to hash
 * @return Bucket index in range [0, hash_size-1]
 *
 * Uses simple modulo hashing. The PID is cast to size_t to ensure
 * positive values before taking the modulo.
 *
 * @pre proc_table->hash_size must be > 0 (callers must guard against destroyed
 * tables)
 */
static size_t pid_hash(const struct process_table *proc_table, pid_t pid) {
    return (size_t)pid % proc_table->hash_size;
}

/**
 * @brief Look up a process in the table by its PID
 * @param proc_table Pointer to the process table to search
 * @param pid Process ID to search for
 * @return Pointer to the process structure if found, NULL otherwise
 *
 * Performs O(1) average-case lookup by hashing the PID to determine the
 * bucket, then searching the linked list in that bucket. Returns NULL if
 * the process table is NULL, the table has been destroyed (proc_table->buckets
 * is NULL), the bucket is empty, or the PID is not found.
 */
struct process *ptbl_find(const struct process_table *proc_table, pid_t pid) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return NULL;
    }
    bucket_idx = pid_hash(proc_table, pid);
    if (proc_table->buckets[bucket_idx] == NULL) {
        return NULL;
    }
    /* Search the linked list in this bucket, comparing PIDs */
    return (struct process *)find_elem(proc_table->buckets[bucket_idx], &pid,
                                       offsetof(struct process, pid),
                                       sizeof(pid_t));
}

/**
 * @brief Insert a process into the hash table
 * @param proc_table Pointer to the process table
 * @param proc Pointer to the process structure to insert
 *
 * Adds the process to the appropriate bucket based on its PID hash.
 * If the bucket doesn't exist, creates a new linked list for it.
 * Exits with an error if memory allocation fails.
 *
 * @note If a process with the same PID is already present in the table, the
 *       existing entry is left unchanged and the new process is not inserted
 *       (duplicate PIDs are ignored).
 * @note Safe to call when proc_table is NULL or the table has been destroyed
 *       (proc_table->buckets is NULL): the call is a no-op in both cases.
 */
void ptbl_add(struct process_table *proc_table, struct process *proc) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL || proc == NULL) {
        return;
    }
    bucket_idx = pid_hash(proc_table, proc->pid);
    if (proc_table->buckets[bucket_idx] == NULL) {
        /* Bucket is empty; create new linked list for this bucket */
        if ((proc_table->buckets[bucket_idx] =
                 (struct list *)malloc(sizeof(struct list))) == NULL) {
            fprintf(stderr, "Memory allocation failed for the process list\n");
            exit(EXIT_FAILURE);
        }
        init_list(proc_table->buckets[bucket_idx]);
    }
    /* Verify process doesn't already exist before adding */
    if (find_elem(proc_table->buckets[bucket_idx], &proc->pid,
                  offsetof(struct process, pid), sizeof(pid_t)) == NULL) {
        add_elem(proc_table->buckets[bucket_idx], proc);
    }
}

/**
 * @brief Remove a process from the hash table by PID
 * @param proc_table Pointer to the process table
 * @param pid Process ID of the process to remove
 * @return 0 on successful deletion, 1 if process not found, table is NULL,
 *         or table has been destroyed
 *
 * Locates the process by PID, removes its node from the linked list, and
 * frees the node. If removing the last node from a bucket, also frees the
 * bucket's linked list structure. The process data itself is freed by this
 * operation.
 */
int ptbl_erase(struct process_table *proc_table, pid_t pid) {
    struct list_node *node;
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return 1;
    }
    bucket_idx = pid_hash(proc_table, pid);
    if (proc_table->buckets[bucket_idx] == NULL) {
        return 1; /* Bucket is empty */
    }
    node = find_node(proc_table->buckets[bucket_idx], &pid,
                     offsetof(struct process, pid), sizeof(pid_t));
    if (node == NULL) {
        return 1; /* Process not found in bucket */
    }
    /* Remove node and free its data */
    free_node(proc_table->buckets[bucket_idx], node);
    /* If bucket is now empty, free the list structure */
    if (is_empty_list(proc_table->buckets[bucket_idx])) {
        free(proc_table->buckets[bucket_idx]);
        proc_table->buckets[bucket_idx] = NULL;
    }
    return 0;
}

/**
 * @brief Remove stale entries from the hash table
 * @param proc_table Pointer to the process table to clean up
 * @param active_list Pointer to the list of currently active processes
 *
 * Iterates through all buckets in the hash table and removes any process
 * entries whose PIDs are not present in the active_list. Also removes any
 * NULL-data nodes encountered. This prevents unbounded growth of the hash
 * buckets when tracked processes terminate.
 * The process data for removed entries is freed.
 *
 * @note Safe to call with NULL pointer (does nothing)
 * @note Safe to call on a destroyed table (proc_table->buckets is NULL): does
 * nothing
 */
void ptbl_remove_stale(struct process_table *proc_table,
                       const struct list *active_list) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return;
    }
    for (bucket_idx = 0; bucket_idx < proc_table->hash_size; bucket_idx++) {
        struct list_node *node, *next_node;
        if (proc_table->buckets[bucket_idx] == NULL) {
            continue;
        }
        for (node = first_node(proc_table->buckets[bucket_idx]); node != NULL;
             node = next_node) {
            next_node = node->next;
            if (node->data == NULL) {
                /* Defensive: remove phantom NULL-data nodes */
                free_node(proc_table->buckets[bucket_idx], node);
            } else {
                pid_t pid = ((const struct process *)node->data)->pid;
                if (find_elem(active_list, &pid, offsetof(struct process, pid),
                              sizeof(pid_t)) == NULL) {
                    free_node(proc_table->buckets[bucket_idx], node);
                }
            }
        }
        if (is_empty_list(proc_table->buckets[bucket_idx])) {
            free(proc_table->buckets[bucket_idx]);
            proc_table->buckets[bucket_idx] = NULL;
        }
    }
}

/**
 * @brief Destroy the hash table and free all associated memory
 * @param proc_table Pointer to the process table to destroy
 *
 * Iterates through all buckets, destroying each linked list and its
 * contents (including process data), then frees the bucket array itself.
 * After destruction, the buckets array pointer is set to NULL.
 *
 * @note Safe to call with NULL pointer (does nothing)
 */
void ptbl_destroy(struct process_table *proc_table) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return;
    }
    /* Free each bucket's linked list and its contents */
    for (bucket_idx = 0; bucket_idx < proc_table->hash_size; bucket_idx++) {
        if (proc_table->buckets[bucket_idx] != NULL) {
            destroy_list(proc_table->buckets[bucket_idx]);
            free(proc_table->buckets[bucket_idx]);
        }
    }
    /* Free the bucket array itself */
    free((void *)proc_table->buckets);
    proc_table->buckets = NULL;
    proc_table->hash_size = 0;
}
