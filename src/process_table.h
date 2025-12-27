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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_iterator.h"
#include <stddef.h>

/**
 * @struct process_table
 * @brief Structure representing a hash table for process storage
 */
struct process_table
{
    /** Array of pointers to linked lists for storing processes */
    struct list **table;
    /** Size of the hash table for the process table */
    size_t hashsize;
};

/**
 * @brief Initialize a process table with the given hash size
 * @param pt Pointer to the process table structure to initialize
 * @param hashsize Size of the hash table
 */
void process_table_init(struct process_table *pt, size_t hashsize);

/**
 * @brief Find a process in the process table based on the PID
 * @param pt Pointer to the process table to search in
 * @param procptr Pointer to a pid_t variable or a struct process object
 *                representing the target process
 * @return Pointer to the found process, or NULL if not found
 */
struct process *process_table_find(const struct process_table *pt,
                                   const void *procptr);

/**
 * @brief Add a process to the process table
 * @param pt The process table to add the process to
 * @param p The process to add
 * @note This function should only be called when p does not exist in pt
 */
void process_table_add(struct process_table *pt, struct process *p);

/**
 * @brief Delete a process from the process table based on the PID
 * @param pt Pointer to the process table to delete the process from
 * @param procptr Pointer to a pid_t variable or a struct process object
 *                representing the target process
 * @return 0 if deletion is successful, 1 if process not found
 */
int process_table_del(struct process_table *pt, const void *procptr);

/**
 * @brief Destroy the process table and free up the memory
 * @param pt Pointer to the process table to destroy
 */
void process_table_destroy(struct process_table *pt);

#endif
