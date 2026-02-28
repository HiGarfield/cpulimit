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
    lst->first = lst->last = NULL;
    lst->count = 0;
}

/**
 * @brief Append an element to the end of the list
 * @param lst Pointer to the list
 * @param elem Pointer to the data element to add
 * @return Pointer to the newly created node, or NULL if @p lst is NULL
 *
 * Creates a new node containing the data pointer and appends it to the end
 * of the list in O(1) time. The list stores only the pointer; ownership of
 * the data remains with the caller.
 *
 * @note On memory allocation failure for the new node, this function
 *       terminates the process and does not return to the caller.
 */
struct list_node *add_elem(struct list *lst, void *elem) {
    struct list_node *new_node;
    if (lst == NULL) {
        return NULL;
    }
    new_node = (struct list_node *)malloc(sizeof(struct list_node));
    if (new_node == NULL) {
        fprintf(stderr, "Memory allocation failed for the new list node\n");
        exit(EXIT_FAILURE);
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
void delete_node(struct list *lst, struct list_node *node) {
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
 * @note Safe to call with NULL list or node; does nothing when either is NULL.
 */
void destroy_node(struct list *lst, struct list_node *node) {
    if (lst == NULL || node == NULL) {
        return;
    }
    free(node->data);
    delete_node(lst, node);
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
 * @brief Get the number of elements in the list
 * @param lst Pointer to the list
 * @return Number of elements, or 0 if list is NULL
 *
 * Returns the count in O(1) time as it is maintained during operations.
 */
size_t get_list_count(const struct list *lst) {
    return lst != NULL ? lst->count : 0;
}

/**
 * @brief Get the first node in the list
 * @param lst Pointer to the list
 * @return Pointer to the first node, or NULL if list is empty or NULL
 *
 * Provides O(1) access to the list head. Use for starting forward iteration.
 */
struct list_node *first_node(const struct list *lst) {
    return lst != NULL ? lst->first : NULL;
}

/**
 * @brief Search for a node by comparing a field in its data
 * @param lst Pointer to the list to search
 * @param elem Pointer to the value to compare against
 * @param offset Byte offset of the field to compare within the data structure
 * @param length Number of bytes to compare
 * @return Pointer to the first matching node, or NULL if not found
 *
 * Performs linear search comparing length bytes starting at offset within
 * each node's data against the provided value. Uses memcmp() for comparison.
 * Useful for finding nodes by a specific field (e.g., PID in a process struct).
 *
 * @note Returns NULL if list is NULL, elem is NULL, or length is 0
 */
struct list_node *locate_node(const struct list *lst, const void *elem,
                              size_t offset, size_t length) {
    struct list_node *current_node;

    if (lst == NULL || elem == NULL || length == 0) {
        return NULL;
    }

    /* Traverse list and compare specified field in each node's data */
    for (current_node = lst->first; current_node != NULL;
         current_node = current_node->next) {
        if (current_node->data == NULL) {
            continue;
        }
        if (memcmp((const char *)current_node->data + offset, elem, length) ==
            0) {
            return current_node;
        }
    }

    return NULL;
}

/**
 * @brief Search for an element by comparing a field in its data
 * @param lst Pointer to the list to search
 * @param elem Pointer to the value to compare against
 * @param offset Byte offset of the field to compare within the data structure
 * @param length Number of bytes to compare
 * @return Pointer to the matching element's data, or NULL if not found
 *
 * Convenience wrapper around locate_node() that returns the data pointer
 * directly rather than the node. Useful when you need the element itself
 * and don't need to manipulate the node.
 */
void *locate_elem(const struct list *lst, const void *elem, size_t offset,
                  size_t length) {
    struct list_node *node = locate_node(lst, elem, offset, length);
    return node != NULL ? node->data : NULL;
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
    struct list_node *current, *next;
    if (lst == NULL || lst->count == 0) {
        return;
    }
    /* Traverse and free all nodes, preserving data pointers */
    for (current = lst->first; current != NULL; current = next) {
        next = current->next;
        free(current);
    }
    /* Reset list to empty state */
    lst->first = lst->last = NULL;
    lst->count = 0;
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
    struct list_node *current, *next;
    if (lst == NULL || lst->count == 0) {
        return;
    }
    /* Traverse and free all nodes and their data */
    for (current = lst->first; current != NULL; current = next) {
        next = current->next;
        free(current->data);
        free(current);
    }
    /* Reset list to empty state */
    lst->first = lst->last = NULL;
    lst->count = 0;
}
