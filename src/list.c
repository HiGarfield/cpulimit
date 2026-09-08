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
 * @brief Initialize an empty doubly linked list
 * @param lst Pointer to the list structure to initialize
 *
 * Sets first and last pointers to NULL and count to 0, preparing the list
 * for use. Safe to call with NULL pointer (does nothing).
 */
void init_list(struct list *lst) {
    if (lst == NULL) {
        return;
    }
    memset(lst, 0, sizeof(*lst));
}

/**
 * @brief Append an element to the end of the list
 * @param lst Pointer to the list
 * @param elem Pointer to the data element to add
 * @return Pointer to the newly created node, or NULL if lst is NULL or
 *         memory allocation for the new node fails
 *
 * Creates a new node containing the data pointer and appends it to the end
 * of the list in O(1) time. The list stores only the pointer; ownership of
 * the data remains with the caller.
 *
 * @note On memory allocation failure the caller must decide how to proceed;
 *       this function returns NULL rather than terminating the process, so
 *       an out-of-memory condition in the limiting loop can be turned into a
 *       clean SIGCONT to the group instead of an abrupt exit that strands a
 *       stopped process.
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
        /* Empty list: new node becomes both first and last */
        lst->first = lst->last = new_node;
    } else {
        /* Non-empty list: append to end */
        lst->last->next = new_node;
        lst->last = new_node;
    }
    lst->count++;
    return new_node;
}

/**
 * @brief Remove a node from the list without freeing its data
 * @param lst Pointer to the list
 * @param node Pointer to the node to remove
 *
 * Unlinks the node from the list and frees the node structure itself, but
 * does not free the data pointer. Use this when the data is managed
 * externally or when multiple references to the data exist.
 *
 * @note Safe to call with NULL list or node (does nothing)
 */
void delete_list_node(struct list *lst, struct list_node *node) {
    if (lst == NULL || node == NULL || lst->count == 0) {
        return;
    }

    /* Update previous node's next pointer, or update list head */
    if (node->previous != NULL) {
        node->previous->next = node->next;
    } else {
        lst->first = node->next;
    }

    /* Update next node's previous pointer, or update list tail */
    if (node->next != NULL) {
        node->next->previous = node->previous;
    } else {
        lst->last = node->previous;
    }

    lst->count--;
    free(node);
}

/**
 * @brief Remove a node from the list and free its data
 * @param lst Pointer to the list
 * @param node Pointer to the node to remove
 *
 * Unlinks the node from the list, frees the data pointer using free(),
 * then frees the node structure. Use this only when the data was allocated
 * with malloc() and has no other references.
 *
 * @note Safe to call with NULL list or node; does nothing when either is NULL
 */
void destroy_list_node(struct list *lst, struct list_node *node) {
    if (lst == NULL || node == NULL) {
        return;
    }
    free(node->data);
    delete_list_node(lst, node);
}

/**
 * @brief Check if the list is empty
 * @param lst Pointer to the list
 * @return 1 if the list is empty or NULL, 0 otherwise
 *
 * Provides O(1) emptiness check by examining the count field.
 */
int is_empty_list(const struct list *lst) {
    return lst == NULL || lst->count == 0;
}

/**
 * @brief Get the first node in the list
 * @param lst Pointer to the list
 * @return Pointer to the first node, or NULL if list is empty or NULL
 *
 * Provides O(1) access to the list head. Use for starting forward iteration.
 */
struct list_node *first_list_node(const struct list *lst) {
    return lst != NULL ? lst->first : NULL;
}

/**
 * @brief Search for a process in the list by its PID
 * @param lst Pointer to the list to search
 * @param pid Process ID to search for
 * @return Pointer to the matching process structure, or NULL if not found
 *
 * Performs linear search comparing the pid field of each node's data.
 * The list is expected to contain struct process pointers.
 *
 * @note Returns NULL if list is NULL
 */
struct process *find_process_in_list_by_pid(const struct list *lst, pid_t pid) {
    struct list_node *current_node;

    if (lst == NULL) {
        return NULL;
    }

    /* Traverse list and compare PID directly in each node's data */
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
 * @brief Remove all nodes from the list without freeing node data
 * @param lst Pointer to the list to clear
 *
 * Frees all node structures but leaves the data pointers intact. Use this
 * when the data is managed externally or when you need to preserve the data
 * while resetting the list. After clearing, the list is empty but can be
 * reused.
 *
 * @note Safe to call with NULL list (does nothing)
 */
void clear_list(struct list *lst) {
    struct list_node *current_node, *next_node;
    if (lst == NULL || lst->count == 0) {
        return;
    }
    /* Traverse and free all nodes, preserving data pointers */
    for (current_node = lst->first; current_node != NULL;
         current_node = next_node) {
        next_node = current_node->next;
        free(current_node);
    }
    /* Reset list to empty state */
    init_list(lst);
}

/**
 * @brief Remove all nodes from the list and free their data
 * @param lst Pointer to the list to destroy
 *
 * Frees all node structures and their associated data pointers using free().
 * Use this only when all data was allocated with malloc() and has no other
 * references. After destruction, the list is empty but can be reused.
 *
 * @note Safe to call with NULL list (does nothing)
 */
void destroy_list(struct list *lst) {
    struct list_node *current_node, *next_node;
    if (lst == NULL || lst->count == 0) {
        return;
    }
    /* Traverse and free all nodes and their data */
    for (current_node = lst->first; current_node != NULL;
         current_node = next_node) {
        next_node = current_node->next;
        free(current_node->data);
        free(current_node);
    }
    /* Reset list to empty state */
    init_list(lst);
}
