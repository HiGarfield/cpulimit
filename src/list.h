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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>

/**
 * @struct list_node
 * @brief Structure representing a node in a doubly linked list
 */
struct list_node {
    /** Pointer to the data content of the node */
    void *data;
    /** Pointer to the previous node in the list */
    struct list_node *previous;
    /** Pointer to the next node in the list */
    struct list_node *next;
};

/**
 * @struct list
 * @brief Structure representing a doubly linked list
 */
struct list {
    /** Pointer to the first node in the list */
    struct list_node *first;
    /** Pointer to the last node in the list */
    struct list_node *last;
    /** Count of elements in the list */
    size_t count;
};

/**
 * @brief Initialize a doubly linked list
 * @param l Pointer to the list structure
 */
void init_list(struct list *l);

/**
 * @brief Add a new element to the end of the list
 * @param l Pointer to the list structure
 * @param elem Pointer to the element to add
 * @return Pointer to the newly created node, or NULL on failure
 */
struct list_node *add_elem(struct list *l, void *elem);

/**
 * @brief Remove a specified node from the list
 * @param l Pointer to the list from which to remove the node
 * @param node Pointer to the node to remove
 * @note This function only removes the node, not its data
 */
void delete_node(struct list *l, struct list_node *node);

/**
 * @brief Remove a node from the list and free its data
 * @param l Pointer to the list from which to remove the node
 * @param node Pointer to the node to remove
 * @note This function should only be used when the node's data is
 *       dynamically allocated.
 */
void destroy_node(struct list *l, struct list_node *node);

/**
 * @brief Check if a list is empty
 * @param l Pointer to the list structure
 * @return 1 if the list is empty, 0 otherwise
 */
int is_empty_list(const struct list *l);

/**
 * @brief Get the number of elements in the list
 * @param l Pointer to the list structure
 * @return Number of elements in the list
 */
size_t get_list_count(const struct list *l);

/**
 * @brief Get the first node in the list
 * @param l Pointer to the list structure
 * @return Pointer to the first node, or NULL if the list is empty
 */
struct list_node *first_node(const struct list *l);

/**
 * @brief Locate a node in the list by comparing a portion of its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the data to compare
 * @param offset Offset from which to start comparison in the node's data
 * @param length Length of the comparison
 * @return Pointer to the found node, or NULL if not found
 * @note Comparison starts from the specified offset.
 */
struct list_node *locate_node(const struct list *l, const void *elem,
                              size_t offset, size_t length);

/**
 * @brief Locate an element in the list by comparing a portion of its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the data to compare
 * @param offset Offset from which to start comparison in the node's data
 * @param length Length of the comparison
 * @return Pointer to the found node's data (element) if found,
 *         or NULL if not found
 * @note Comparison starts from the specified offset.
 */
void *locate_elem(const struct list *l, const void *elem, size_t offset,
                  size_t length);

/**
 * @brief Clear all nodes from the list without freeing node data
 * @param l Pointer to the list to clear
 * @note This function does not free the node data, only removes the nodes
 */
void clear_list(struct list *l);

/**
 * @brief Clear all nodes from the list and free node data
 * @param l Pointer to the list to destroy
 * @note This function frees both the nodes and their associated data
 */
void destroy_list(struct list *l);

#endif
