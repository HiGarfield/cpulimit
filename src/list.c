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
 * @brief Initialize a doubly linked list
 * @param l Pointer to the list structure
 * @param keysize Size of the key for element comparison
 */
void init_list(struct list *l, size_t keysize) {
    if (l == NULL) {
        return;
    }
    l->first = l->last = NULL;
    l->keysize = keysize;
    l->count = 0;
}

/**
 * @brief Add a new element to the end of the list
 * @param l Pointer to the list structure
 * @param elem Pointer to the element to add
 * @return Pointer to the newly created node, or NULL on failure
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
        l->first = l->last = newnode;
    } else {
        l->last->next = newnode;
        l->last = newnode;
    }
    l->count++;
    return newnode;
}

/**
 * @brief Remove a specified node from the list
 * @param l Pointer to the list from which to remove the node
 * @param node Pointer to the node to remove
 * @note This function only removes the node, not its data
 */
void delete_node(struct list *l, struct list_node *node) {
    if (l == NULL || node == NULL || l->count == 0) {
        return;
    }

    if (node->previous != NULL) {
        node->previous->next = node->next;
    } else {
        l->first = node->next;
    }

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
 * @param l Pointer to the list from which to remove the node
 * @param node Pointer to the node to remove
 * @note This function should only be used when the node's data is
 *       dynamically allocated.
 */
void destroy_node(struct list *l, struct list_node *node) {
    if (node != NULL && node->data != NULL) {
        free(node->data);
    }
    delete_node(l, node);
}

/**
 * @brief Check if a list is empty
 * @param l Pointer to the list structure
 * @return 1 if the list is empty, 0 otherwise
 */
int is_empty_list(const struct list *l) {
    return l == NULL || l->count == 0;
}

/**
 * @brief Get the number of elements in the list
 * @param l Pointer to the list structure
 * @return Number of elements in the list
 */
size_t get_list_count(const struct list *l) {
    return l != NULL ? l->count : 0;
}

/**
 * @brief Get the first node in the list
 * @param l Pointer to the list structure
 * @return Pointer to the first node, or NULL if the list is empty
 */
struct list_node *first_node(const struct list *l) {
    return l != NULL ? l->first : NULL;
}

/**
 * @brief Search for a node in the list by comparing a portion of its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the data to compare
 * @param offset Offset from which to start comparison in the node's data
 * @param length Length of the comparison (0 means use list's keysize)
 * @return Pointer to the found node, or NULL if not found
 * @note Comparison starts from the specified offset. If offset=0, comparison
 *       starts from the beginning of the data. If length=0, the list's keysize
 *       is used for comparison.
 */
static struct list_node *xlocate_node(const struct list *l, const void *elem,
                                      size_t offset, size_t length) {
    struct list_node *cur;
    size_t cmp_len;

    if (l == NULL || elem == NULL) {
        return NULL;
    }

    cmp_len = (length != 0) ? length : l->keysize;
    if (cmp_len == 0) {
        return NULL;
    }

    for (cur = l->first; cur != NULL; cur = cur->next) {
        if (memcmp((const char *)cur->data + offset, elem, cmp_len) == 0) {
            return cur;
        }
    }

    return NULL;
}

/**
 * @brief Locate a node in the list by comparing its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the node to locate
 * @return Pointer to the node if found; NULL if not found
 * @note Comparison starts from the beginning of the data, and the list's
 *       keysize is used for comparison.
 */
struct list_node *locate_node(const struct list *l, const void *elem) {
    return xlocate_node(l, elem, 0, 0);
}

/**
 * @brief Locate an element in the list by comparing its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the element to locate
 * @return Pointer to the element's data if found; NULL if not found
 * @note Comparison starts from the specified offset. If offset=0, comparison
 *       starts from the beginning of the data. If length=0, the list's keysize
 *       is used for comparison.
 */
static void *xlocate_elem(const struct list *l, const void *elem, size_t offset,
                          size_t length) {
    struct list_node *node = xlocate_node(l, elem, offset, length);
    return node != NULL ? node->data : NULL;
}

/**
 * @brief Locate an element in the list by comparing its data
 * @param l Pointer to the list to search
 * @param elem Pointer to the element to locate
 * @return Pointer to the element's data if found; NULL if not found
 * @note Comparison starts from the beginning of the data, and the list's
 *       keysize is used for comparison.
 */
void *locate_elem(const struct list *l, const void *elem) {
    return xlocate_elem(l, elem, 0, 0);
}

/**
 * @brief Clear all nodes from the list, optionally freeing node data
 * @param l Pointer to the list structure
 * @param free_data Flag indicating whether to free node data (1) or not (0)
 */
static void clear_all_list_nodes(struct list *l, int free_data) {
    struct list_node *current, *next;
    if (l == NULL || l->count == 0) {
        return;
    }
    for (current = l->first; current != NULL; current = next) {
        next = current->next;
        if (free_data && current->data != NULL) {
            free(current->data);
        }
        free(current);
    }
    l->first = l->last = NULL;
    l->count = 0;
}

/**
 * @brief Clear all nodes from the list without freeing node data
 * @param l Pointer to the list to clear
 * @note This function does not free the node data, only removes the nodes
 */
void clear_list(struct list *l) {
    clear_all_list_nodes(l, 0);
}

/**
 * @brief Clear all nodes from the list and free node data
 * @param l Pointer to the list to destroy
 * @note This function frees both the nodes and their associated data
 */
void destroy_list(struct list *l) {
    clear_all_list_nodes(l, 1);
}
