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

#ifndef __LIST_H
#define __LIST_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>

/**
 * @struct list_node
 * @brief Node in a doubly linked list
 *
 * Each node contains a data pointer and links to adjacent nodes, enabling
 * bidirectional traversal. The data pointer is managed by the caller; the
 * list only stores references and does not take ownership by default.
 */
struct list_node {
    /**
     * Pointer to the user-provided data element.
     * The list stores only the pointer; memory management of the data
     * is the caller's responsibility unless using destroy operations.
     */
    void *data;

    /**
     * Pointer to the previous node in the list.
     * NULL if this is the first node.
     */
    struct list_node *previous;

    /**
     * Pointer to the next node in the list.
     * NULL if this is the last node.
     */
    struct list_node *next;
};

/**
 * @struct list
 * @brief Doubly linked list with head, tail, and count tracking
 *
 * This structure provides O(1) insertion at the end, O(1) access to first
 * and last elements, and O(1) element counting. Supports bidirectional
 * traversal through the previous and next pointers in each node.
 */
struct list {
    /**
     * Pointer to the first node in the list.
     * NULL if the list is empty.
     */
    struct list_node *first;

    /**
     * Pointer to the last node in the list.
     * NULL if the list is empty.
     */
    struct list_node *last;

    /**
     * Number of elements currently in the list.
     * Maintained automatically during add and delete operations.
     */
    size_t count;
};

/**
 * @brief Initialize an empty doubly linked list
 * @param list_ptr Pointer to the list structure to initialize
 *
 * Sets first and last pointers to NULL and count to 0, preparing the list
 * for use. Safe to call with NULL pointer (does nothing).
 */
void init_list(struct list *list_ptr);

/**
 * @brief Append an element to the end of the list
 * @param list_ptr Pointer to the list
 * @param elem Pointer to the data element to add
 * @return Pointer to the newly created node, or NULL if @p list_ptr is NULL
 *
 * Creates a new node containing the data pointer and appends it to the end
 * of the list in O(1) time. The list stores only the pointer; ownership of
 * the data remains with the caller.
 *
 * @note On memory allocation failure for the new node, this function
 *       terminates the process and does not return to the caller.
 */
struct list_node *add_elem(struct list *list_ptr, void *elem);

/**
 * @brief Remove a node from the list without freeing its data
 * @param list_ptr Pointer to the list
 * @param node Pointer to the node to remove
 *
 * Unlinks the node from the list and frees the node structure itself, but
 * does not free the data pointer. Use this when the data is managed
 * externally or when multiple references to the data exist.
 *
 * @note Safe to call with NULL list or node (does nothing)
 */
void delete_node(struct list *list_ptr, struct list_node *node);

/**
 * @brief Remove a node from the list and free its data
 * @param list_ptr Pointer to the list
 * @param node Pointer to the node to remove
 *
 * Unlinks the node from the list, frees the data pointer using free(),
 * then frees the node structure. Use this only when the data was allocated
 * with malloc() and has no other references.
 *
 * @note Safe to call with NULL list or node (does nothing)
 */
void destroy_node(struct list *list_ptr, struct list_node *node);

/**
 * @brief Check if the list is empty
 * @param list_ptr Pointer to the list
 * @return 1 if the list is empty or NULL, 0 otherwise
 *
 * Provides O(1) emptiness check by examining the count field.
 */
int is_empty_list(const struct list *list_ptr);

/**
 * @brief Get the number of elements in the list
 * @param list_ptr Pointer to the list
 * @return Number of elements, or 0 if list is NULL
 *
 * Returns the count in O(1) time as it is maintained during operations.
 */
size_t get_list_count(const struct list *list_ptr);

/**
 * @brief Get the first node in the list
 * @param list_ptr Pointer to the list
 * @return Pointer to the first node, or NULL if list is empty or NULL
 *
 * Provides O(1) access to the list head. Use for starting forward iteration.
 */
struct list_node *first_node(const struct list *list_ptr);

/**
 * @brief Search for a node by comparing a field in its data
 * @param list_ptr Pointer to the list to search
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
struct list_node *locate_node(const struct list *list_ptr, const void *elem,
                              size_t offset, size_t length);

/**
 * @brief Search for an element by comparing a field in its data
 * @param list_ptr Pointer to the list to search
 * @param elem Pointer to the value to compare against
 * @param offset Byte offset of the field to compare within the data structure
 * @param length Number of bytes to compare
 * @return Pointer to the matching element's data, or NULL if not found
 *
 * Convenience wrapper around locate_node() that returns the data pointer
 * directly rather than the node. Useful when you need the element itself
 * and don't need to manipulate the node.
 */
void *locate_elem(const struct list *list_ptr, const void *elem, size_t offset,
                  size_t length);

/**
 * @brief Remove all nodes from the list without freeing node data
 * @param list_ptr Pointer to the list to clear
 *
 * Frees all node structures but leaves the data pointers intact. Use this
 * when the data is managed externally or when you need to preserve the data
 * while resetting the list. After clearing, the list is empty but can be
 * reused.
 *
 * @note Safe to call with NULL list (does nothing)
 */
void clear_list(struct list *list_ptr);

/**
 * @brief Remove all nodes from the list and free their data
 * @param list_ptr Pointer to the list to destroy
 *
 * Frees all node structures and their associated data pointers using free().
 * Use this only when all data was allocated with malloc() and has no other
 * references. After destruction, the list is empty but can be reused.
 *
 * @note Safe to call with NULL list (does nothing)
 */
void destroy_list(struct list *list_ptr);

#ifdef __cplusplus
}
#endif

#endif
