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

#ifndef CPULIMIT_LIST_H
#define CPULIMIT_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "process_iterator.h"

#include <stddef.h>
#include <sys/types.h>

/**
 * @struct list_node
 * @brief Node of a doubly linked list.
 */
struct list_node {
    /**
     * @brief Borrowed pointer to the user's data.
     */
    void *data;

    /**
     * @brief Previous node, or NULL at the head.
     */
    struct list_node *previous;

    /**
     * @brief Next node, or NULL at the tail.
     */
    struct list_node *next;
};

/**
 * @struct list
 * @brief Doubly linked list with head, tail and element count.
 */
struct list {
    /**
     * @brief First node, or NULL when empty.
     */
    struct list_node *first;

    /**
     * @brief Last node, or NULL when empty.
     */
    struct list_node *last;

    /**
     * @brief Number of nodes in the list.
     */
    size_t count;
};

/**
 * @brief Initialize an empty doubly linked list.
 *
 * @param lst List to initialize.
 *
 * @note A NULL lst is a no-op.
 */
void init_list(struct list *lst);

/**
 * @brief Append an element to the end of the list.
 *
 * @param lst List to append to.
 * @param elem Data pointer to store.
 * @return The new node, or NULL for a NULL lst or an allocation failure.
 *
 * @note The list borrows elem; ownership stays with the caller. Returning
 *       NULL lets the caller resume a stopped group instead of exiting.
 */
struct list_node *add_list_elem(struct list *lst, void *elem);

/**
 * @brief Remove a node without freeing its data.
 *
 * @param lst List to modify.
 * @param node Node to unlink and free.
 *
 * @note A NULL lst or node is a no-op.
 */
void delete_list_node(struct list *lst, struct list_node *node);

/**
 * @brief Remove a node and free its data.
 *
 * @param lst List to modify.
 * @param node Node to unlink and free.
 *
 * @note Only safe when the data was allocated with malloc() and is not
 *       referenced elsewhere. A NULL lst or node is a no-op.
 */
void destroy_list_node(struct list *lst, struct list_node *node);

/**
 * @brief Check whether the list has no elements.
 *
 * @param lst List to query.
 * @return 1 when empty or NULL, 0 otherwise.
 */
int is_empty_list(const struct list *lst);

/**
 * @brief Get the first node of the list.
 *
 * @param lst List to query.
 * @return The first node, or NULL when empty or NULL.
 */
struct list_node *first_list_node(const struct list *lst);

/**
 * @brief Find a process in the list by PID.
 *
 * @param lst List of struct process pointers.
 * @param pid PID to find.
 * @return The matching process, or NULL when absent or lst is NULL.
 */
struct process *find_process_in_list_by_pid(const struct list *lst, pid_t pid);

/**
 * @brief Remove all nodes without freeing their data.
 *
 * @param lst List to clear.
 *
 * @note Ownership of the data stays with the caller. A NULL lst is a no-op.
 */
void clear_list(struct list *lst);

/**
 * @brief Remove all nodes and free their data.
 *
 * @param lst List to destroy.
 *
 * @note Only safe when every data pointer came from malloc() and has no
 *       other owner. A NULL lst is a no-op.
 */
void destroy_list(struct list *lst);

#ifdef __cplusplus
}
#endif

#endif
