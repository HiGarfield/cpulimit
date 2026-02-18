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

#ifndef __PROCESS_TABLE_H
#define __PROCESS_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_iterator.h"

#include <stddef.h>
#include <sys/types.h>

/**
 * @struct process_table
 * @brief Hash table for efficient process storage and lookup by PID
 *
 * This structure implements a hash table using separate chaining to store
 * process structures. Each bucket in the table contains a linked list that
 * holds processes whose PIDs hash to the same index. This provides O(1)
 * average-case lookup, insertion, and deletion operations.
 */
struct process_table {
    /**
     * Array of linked list pointers forming the hash table buckets.
     * Each bucket is NULL if empty, or points to a linked list of processes.
     * Processes are stored in separate chaining lists to handle collisions.
     */
    struct list **table;

    /**
     * Number of buckets in the hash table.
     * PIDs are hashed modulo this value to determine bucket placement.
     * Larger sizes reduce collision probability but increase memory usage.
     */
    size_t hashsize;
};

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
void process_table_init(struct process_table *pt, size_t hashsize);

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
struct process *process_table_find(const struct process_table *pt, pid_t pid);

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
 *       existing entry is left unchanged and the new process is not inserted
 *       (duplicate PIDs are ignored).
 */
void process_table_add(struct process_table *pt, struct process *p);

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
int process_table_del(struct process_table *pt, pid_t pid);

/**
 * @brief Remove stale entries from the hash table
 * @param pt Pointer to the process table to clean up
 * @param active_list Pointer to the list of currently active processes
 *
 * Iterates through all buckets in the hash table and removes any process
 * entries whose PIDs are not present in the active_list. This prevents
 * unbounded growth of the hash table when tracked processes terminate.
 * The process data for removed entries is freed.
 *
 * @note Safe to call with NULL pointer (does nothing)
 */
void process_table_remove_stale(struct process_table *pt,
                                const struct list *active_list);

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
void process_table_destroy(struct process_table *pt);

#ifdef __cplusplus
}
#endif

#endif
