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

#ifndef CPULIMIT_PROCESS_TABLE_H
#define CPULIMIT_PROCESS_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "process_iterator.h"

#include <stddef.h>
#include <sys/types.h>

/**
 * @struct process_table
 * @brief Hash buckets for process lookup by PID.
 */
struct process_table {
    /**
     * @brief Array of chained lists, one bucket per hash value.
     */
    struct list **buckets;

    /**
     * @brief Number of buckets.
     */
    size_t hash_size;
};

/**
 * @brief Initialize a process hash table.
 *
 * @param proc_table Table to initialize.
 * @param hash_size Number of buckets.
 * @return 0 on success, -1 on a NULL table, an overflowing bucket array or
 *         an allocation failure; never calls exit().
 *
 * @note The caller owns proc_table and must destroy it after success.
 */
int init_process_table(struct process_table *proc_table, size_t hash_size);

/**
 * @brief Look up a process by PID.
 *
 * @param proc_table Table to search.
 * @param pid PID to find.
 * @return The stored process, or NULL when absent or the table is unusable.
 */
struct process *find_in_process_table(const struct process_table *proc_table,
                                      pid_t pid);

/**
 * @brief Insert a process into the hash table.
 *
 * @param proc_table Table to modify.
 * @param proc Process to insert.
 * @return 0 on success or a no-op, -1 on an allocation failure.
 *
 * @note Ownership passes to the table only when the record is inserted. A
 *       duplicate PID, an unusable table or a -1 return leaves proc owned
 *       by the caller, which must free it.
 */
int add_to_process_table(struct process_table *proc_table,
                         struct process *proc);

/**
 * @brief Remove a process and free its storage.
 *
 * @param proc_table Table to modify.
 * @param pid PID to remove.
 * @return 0 when deleted, 1 when absent or the table is unusable.
 */
int delete_from_process_table(struct process_table *proc_table, pid_t pid);

/**
 * @brief Drop table entries absent from the current active list.
 *
 * @param proc_table Table to clean.
 * @param active_list PIDs still present in this cycle.
 *
 * @note A NULL or destroyed table is a no-op.
 */
void remove_stale_from_process_table(struct process_table *proc_table,
                                     const struct list *active_list);

/**
 * @brief Free the table, its buckets and every stored process.
 *
 * @param proc_table Table to destroy.
 *
 * @note A NULL table is a no-op. The bucket pointer is cleared.
 */
void destroy_process_table(struct process_table *proc_table);

#ifdef __cplusplus
}
#endif

#endif
