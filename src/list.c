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

#include "list.h"

#include "process_iterator.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initialize an empty doubly linked list.
 *
 * @param lst List to initialize.
 *
 * @note A NULL lst is a no-op.
 */
void init_list(struct list *lst) {
    if (lst == NULL) {
        return;
    }
    memset(lst, 0, sizeof(*lst));
}

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
struct list_node *add_list_elem(struct list *lst, void *elem) {
    struct list_node *new_node;
    if (lst == NULL) {
        return NULL;
    }
    new_node = (struct list_node *)malloc(sizeof(struct list_node));
    if (new_node == NULL) {
        fprintf(stderr, "Memory allocation failed for the new list node\n");
        return NULL;
    }
    new_node->data = elem;
    new_node->previous = lst->last;
    new_node->next = NULL;
    if (lst->count == 0) {
        lst->first = lst->last = new_node;
    } else {
        lst->last->next = new_node;
        lst->last = new_node;
    }
    lst->count++;
    return new_node;
}

/**
 * @brief Remove a node without freeing its data.
 *
 * @param lst List to modify.
 * @param node Node to unlink and free.
 *
 * @note A NULL lst or node is a no-op.
 */
void delete_list_node(struct list *lst, struct list_node *node) {
    if (lst == NULL || node == NULL || lst->count == 0) {
        return;
    }

    if (node->previous != NULL) {
        node->previous->next = node->next;
    } else {
        lst->first = node->next;
    }

    if (node->next != NULL) {
        node->next->previous = node->previous;
    } else {
        lst->last = node->previous;
    }

    lst->count--;
    free(node);
}

/**
 * @brief Remove a node and free its data.
 *
 * @param lst List to modify.
 * @param node Node to unlink and free.
 *
 * @note Only safe when the data was allocated with malloc() and is not
 *       referenced elsewhere. A NULL lst or node is a no-op.
 */
void destroy_list_node(struct list *lst, struct list_node *node) {
    if (lst == NULL || node == NULL) {
        return;
    }
    free(node->data);
    delete_list_node(lst, node);
}

/**
 * @brief Check whether the list has no elements.
 *
 * @param lst List to query.
 * @return 1 when empty or NULL, 0 otherwise.
 */
int is_empty_list(const struct list *lst) {
    return lst == NULL || lst->count == 0;
}

/**
 * @brief Get the first node of the list.
 *
 * @param lst List to query.
 * @return The first node, or NULL when empty or NULL.
 */
struct list_node *first_list_node(const struct list *lst) {
    return lst != NULL ? lst->first : NULL;
}

/**
 * @brief Find a process in the list by PID.
 *
 * @param lst List of struct process pointers.
 * @param pid PID to find.
 * @return The matching process, or NULL when absent or lst is NULL.
 */
struct process *find_process_in_list_by_pid(const struct list *lst, pid_t pid) {
    struct list_node *current_node;

    if (lst == NULL) {
        return NULL;
    }

    for (current_node = lst->first; current_node != NULL;
         current_node = current_node->next) {
        if (current_node->data == NULL) {
            continue;
        }
        if (((const struct process *)current_node->data)->pid == pid) {
            return (struct process *)current_node->data;
        }
    }

    return NULL;
}

/**
 * @brief Remove all nodes without freeing their data.
 *
 * @param lst List to clear.
 *
 * @note Ownership of the data stays with the caller. A NULL lst is a no-op.
 */
void clear_list(struct list *lst) {
    struct list_node *current_node, *next_node;
    if (lst == NULL || lst->count == 0) {
        return;
    }
    for (current_node = lst->first; current_node != NULL;
         current_node = next_node) {
        next_node = current_node->next;
        free(current_node);
    }
    init_list(lst);
}

/**
 * @brief Remove all nodes and free their data.
 *
 * @param lst List to destroy.
 *
 * @note Only safe when every data pointer came from malloc() and has no
 *       other owner. A NULL lst is a no-op.
 */
void destroy_list(struct list *lst) {
    struct list_node *current_node, *next_node;
    if (lst == NULL || lst->count == 0) {
        return;
    }
    for (current_node = lst->first; current_node != NULL;
         current_node = next_node) {
        next_node = current_node->next;
        free(current_node->data);
        free(current_node);
    }
    init_list(lst);
}
