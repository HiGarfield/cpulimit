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
 * @param l Pointer to the list structure to initialize
 *
 * Sets first and last pointers to NULL and count to 0, preparing the list
 * for use. Safe to call with NULL pointer (does nothing).
 */
void init_list(struct list *l) {
    if (l == NULL) {
        return;
    }
    l->first = l->last = NULL;
    l->count = 0;
}

/**
 * @brief Append an element to the end of the list
 * @param l Pointer to the list
 * @param elem Pointer to the data element to add
 * @return Pointer to the newly created node, or NULL if @p l is NULL
 *
 * Creates a new node containing the data pointer and appends it to the end
 * of the list in O(1) time. The list stores only the pointer; ownership of
 * the data remains with the caller. On memory allocation failure this
 * function prints an error message to stderr and terminates the process
 * with exit(EXIT_FAILURE); it does not return NULL in that case.
 */
struct list_node *add_elem(struct list *l, void *elem) {
    struct list_node *newnode;
    if (l == NULL) {
        return NULL;
    }
    if ((newnode = (struct list_node *)malloc(sizeof(struct list_node))) ==
        NULL) {
        fprintf(stderr, "Memory allocation failed for the new list node\n");
        exit(EXIT_FAILURE);
    }
    newnode->data = elem;
    newnode->previous = l->last;
    newnode->next = NULL;
    if (l->count == 0) {
        /* Empty list: new node becomes both first and last */
        l->first = l->last = newnode;
    } else {
        /* Non-empty list: append to end */
        l->last->next = newnode;
        l->last = newnode;
    }
    l->count++;
    return newnode;
}

/**
 * @brief Remove a node from the list without freeing its data
 * @param l Pointer to the list
 * @param node Pointer to the node to remove
 *
 * Unlinks the node from the list and frees the node structure itself, but
 * does not free the data pointer. Use this when the data is managed
 * externally or when multiple references to the data exist.
 *
 * @note Safe to call with NULL list or node (does nothing)
 */
void delete_node(struct list *l, struct list_node *node) {
    if (l == NULL || node == NULL || l->count == 0) {
        return;
    }

    /* Update previous node's next pointer, or update list head */
    if (node->previous != NULL) {
        node->previous->next = node->next;
    } else {
        l->first = node->next;
    }

    /* Update next node's previous pointer, or update list tail */
    if (node->next != NULL) {
        node->next->previous = node->previous;
    } else {
        l->last = node->previous;
    }

    l->count--;
    free(node);
}

/**
 * @brief Remove a node from the list and free its data
 * @param l Pointer to the list
 * @param node Pointer to the node to remove
 *
 * Unlinks the node from the list, frees the data pointer using free(),
 * then frees the node structure. Use this only when the data was allocated
 * with malloc() and has no other references.
 *
 * @note Safe to call with NULL list or node (does nothing)
 */
void destroy_node(struct list *l, struct list_node *node) {
    if (node != NULL && node->data != NULL) {
        free(node->data);
    }
    delete_node(l, node);
}

/**
 * @brief Check if the list is empty
 * @param l Pointer to the list
 * @return 1 if the list is empty or NULL, 0 otherwise
 *
 * Provides O(1) emptiness check by examining the count field.
 */
int is_empty_list(const struct list *l) {
    return l == NULL || l->count == 0;
}

/**
 * @brief Get the number of elements in the list
 * @param l Pointer to the list
 * @return Number of elements, or 0 if list is NULL
 *
 * Returns the count in O(1) time as it is maintained during operations.
 */
size_t get_list_count(const struct list *l) {
    return l != NULL ? l->count : 0;
}

/**
 * @brief Get the first node in the list
 * @param l Pointer to the list
 * @return Pointer to the first node, or NULL if list is empty or NULL
 *
 * Provides O(1) access to the list head. Use for starting forward iteration.
 */
struct list_node *first_node(const struct list *l) {
    return l != NULL ? l->first : NULL;
}

/**
 * @brief Search for a node by comparing a field in its data
 * @param l Pointer to the list to search
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
struct list_node *locate_node(const struct list *l, const void *elem,
                              size_t offset, size_t length) {
    struct list_node *cur;

    if (l == NULL || elem == NULL || length == 0) {
        return NULL;
    }

    /* Traverse list and compare specified field in each node's data */
    for (cur = l->first; cur != NULL; cur = cur->next) {
        if (cur->data == NULL) {
            continue;
        }
        if (memcmp((const char *)cur->data + offset, elem, length) == 0) {
            return cur;
        }
    }

    return NULL;
}

/**
 * @brief Search for an element by comparing a field in its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the value to compare against
 * @param offset Byte offset of the field to compare within the data structure
 * @param length Number of bytes to compare
 * @return Pointer to the matching element's data, or NULL if not found
 *
 * Convenience wrapper around locate_node() that returns the data pointer
 * directly rather than the node. Useful when you need the element itself
 * and don't need to manipulate the node.
 */
void *locate_elem(const struct list *l, const void *elem, size_t offset,
                  size_t length) {
    struct list_node *node = locate_node(l, elem, offset, length);
    return node != NULL ? node->data : NULL;
}

/**
 * @brief Helper to remove all nodes, optionally freeing data
 * @param l Pointer to the list
 * @param free_data If non-zero, frees each node's data pointer; otherwise
 *                  preserves data
 *
 * Traverses the list and frees all nodes. If free_data is non-zero, also
 * calls free() on each data pointer before freeing the node. Resets the
 * list to empty state (first=NULL, last=NULL, count=0).
 */
static void clear_all_list_nodes(struct list *l, int free_data) {
    struct list_node *current, *next;
    if (l == NULL || l->count == 0) {
        return;
    }
    /* Traverse and free all nodes */
    for (current = l->first; current != NULL; current = next) {
        next = current->next;
        if (free_data && current->data != NULL) {
            free(current->data);
        }
        free(current);
    }
    /* Reset list to empty state */
    l->first = l->last = NULL;
    l->count = 0;
}

/**
 * @brief Remove all nodes from the list without freeing node data
 * @param l Pointer to the list to clear
 *
 * Frees all node structures but leaves the data pointers intact. Use this
 * when the data is managed externally or when you need to preserve the data
 * while resetting the list. After clearing, the list is empty but can be
 * reused.
 *
 * @note Safe to call with NULL list (does nothing)
 */
void clear_list(struct list *l) {
    clear_all_list_nodes(l, 0);
}

/**
 * @brief Remove all nodes from the list and free their data
 * @param l Pointer to the list to destroy
 *
 * Frees all node structures and their associated data pointers using free().
 * Use this only when all data was allocated with malloc() and has no other
 * references. After destruction, the list is empty but can be reused.
 *
 * @note Safe to call with NULL list (does nothing)
 */
void destroy_list(struct list *l) {
    clear_all_list_nodes(l, 1);
}
