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

#ifndef __PROCESS_TABLE_H
#define __PROCESS_TABLE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "list.h"
#include "process_iterator.h"
#include <stddef.h>
#include <sys/types.h>

/**
 * Structure representing a process table.
 */
struct process_table
{
    /* Array of pointers to linked lists for storing processes */
    struct list **table;

    /* Size of the hash table for the process table */
    size_t hashsize;
};

/**
 * Initializes the process table with the given hash size.
 *
 * @param pt The process table to initialize
 * @param hashsize The size of the hash table
 */
void process_table_init(struct process_table *pt, size_t hashsize);

/**
 * Finds a process in the process table based on the PID of the given process.
 *
 * @param pt The process table to search in
 * @param procptr A pointer to a pid_t variable or a struct process object
 *                representing the target process
 * @return A pointer to the found process or NULL if not found
 */
struct process *process_table_find(const struct process_table *pt,
                                   const void *procptr);

/**
 * Adds a process to the process table.
 *
 * @param pt The process table to add the process to
 * @param p The process to add
 */
void process_table_add(struct process_table *pt, struct process *p);

/**
 * Deletes a process from the process table based on the PID of the given
 * process.
 *
 * @param pt The process table to delete the process from
 * @param procptr A pointer to a pid_t variable or a struct process object
 *                representing the target process
 * @return 0 if deletion is successful, 1 if process not found
 */
int process_table_del(struct process_table *pt, const void *procptr);

/**
 * Destroys the process table and frees up the memory.
 *
 * @param pt The process table to destroy
 */
void process_table_destroy(struct process_table *pt);

#endif
