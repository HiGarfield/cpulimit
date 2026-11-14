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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
int init_process_table(struct process_table *proc_table, size_t hash_size) {
    size_t size_max = 0;
    if (proc_table == NULL) {
        return -1;
    }
    memset(proc_table, 0, sizeof(*proc_table));
    if (hash_size == 0) {
        /* A zero-sized table would make the modulo in pid_hash() invalid. */
        hash_size = 1;
    }
    /* Reject an overflowing bucket array before calloc() sees it. */
    size_max--;
    if (hash_size > size_max / sizeof(struct list *)) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        return -1;
    }
    proc_table->hash_size = hash_size;
    proc_table->buckets =
        (struct list **)calloc(proc_table->hash_size, sizeof(struct list *));
    if (proc_table->buckets == NULL) {
        fprintf(stderr, "Memory allocation failed for the process table\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Compute the bucket index for a PID.
 *
 * @param proc_table Table providing the bucket count.
 * @param pid PID to hash.
 * @return Bucket index in [0, hash_size - 1].
 */
static size_t pid_hash(const struct process_table *proc_table, pid_t pid) {
    return (size_t)pid % proc_table->hash_size;
}

/**
 * @brief Look up a process by PID.
 *
 * @param proc_table Table to search.
 * @param pid PID to find.
 * @return The stored process, or NULL when absent or the table is unusable.
 */

struct process *find_in_process_table(const struct process_table *proc_table,
                                      pid_t pid) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return NULL;
    }
    bucket_idx = pid_hash(proc_table, pid);
    if (proc_table->buckets[bucket_idx] == NULL) {
        return NULL;
    }
    return find_process_in_list_by_pid(proc_table->buckets[bucket_idx], pid);
}

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
                         struct process *proc) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL || proc == NULL) {
        return 0;
    }
    bucket_idx = pid_hash(proc_table, proc->pid);
    if (proc_table->buckets[bucket_idx] == NULL) {
        proc_table->buckets[bucket_idx] =
            (struct list *)malloc(sizeof(struct list));
        if (proc_table->buckets[bucket_idx] == NULL) {
            fprintf(stderr, "Memory allocation failed for the process list\n");
            return -1;
        }
        init_list(proc_table->buckets[bucket_idx]);
    }
    if (find_process_in_list_by_pid(proc_table->buckets[bucket_idx],
                                    proc->pid) == NULL) {
        if (add_list_elem(proc_table->buckets[bucket_idx], proc) == NULL) {
            /* Leave the unlinked record to the caller, which frees it. */
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Remove a process and free its storage.
 *
 * @param proc_table Table to modify.
 * @param pid PID to remove.
 * @return 0 when deleted, 1 when absent or the table is unusable.
 */
int delete_from_process_table(struct process_table *proc_table, pid_t pid) {
    struct list_node *node;
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return 1;
    }
    bucket_idx = pid_hash(proc_table, pid);
    if (proc_table->buckets[bucket_idx] == NULL) {
        return 1;
    }
    for (node = proc_table->buckets[bucket_idx]->first; node != NULL;
         node = node->next) {
        if (node->data != NULL &&
            ((const struct process *)node->data)->pid == pid) {
            break;
        }
    }
    if (node == NULL) {
        return 1;
    }
    destroy_list_node(proc_table->buckets[bucket_idx], node);
    if (is_empty_list(proc_table->buckets[bucket_idx])) {
        free(proc_table->buckets[bucket_idx]);
        proc_table->buckets[bucket_idx] = NULL;
    }
    return 0;
}

/**
 * @brief Drop table entries absent from the current active list.
 *
 * @param proc_table Table to clean.
 * @param active_list PIDs still present in this cycle.
 *
 * @note A NULL or destroyed table is a no-op.
 */

void remove_stale_from_process_table(struct process_table *proc_table,
                                     const struct list *active_list) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL ||
        active_list == NULL) {
        return;
    }
    for (bucket_idx = 0; bucket_idx < proc_table->hash_size; bucket_idx++) {
        struct list_node *node, *next_node;
        if (proc_table->buckets[bucket_idx] == NULL) {
            continue;
        }
        for (node = first_list_node(proc_table->buckets[bucket_idx]);
             node != NULL; node = next_node) {
            next_node = node->next;
            if (node->data == NULL) {
                destroy_list_node(proc_table->buckets[bucket_idx], node);
            } else {
                pid_t pid = ((const struct process *)node->data)->pid;
                if (find_process_in_list_by_pid(active_list, pid) == NULL) {
                    destroy_list_node(proc_table->buckets[bucket_idx], node);
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
 * @brief Free the table, its buckets and every stored process.
 *
 * @param proc_table Table to destroy.
 *
 * @note A NULL table is a no-op. The bucket pointer is cleared.
 */
void destroy_process_table(struct process_table *proc_table) {
    size_t bucket_idx;
    if (proc_table == NULL || proc_table->buckets == NULL) {
        return;
    }
    for (bucket_idx = 0; bucket_idx < proc_table->hash_size; bucket_idx++) {
        if (proc_table->buckets[bucket_idx] != NULL) {
            destroy_list(proc_table->buckets[bucket_idx]);
            free(proc_table->buckets[bucket_idx]);
        }
    }
    free((void *)proc_table->buckets);
    proc_table->buckets = NULL;
    proc_table->hash_size = 0;
}
