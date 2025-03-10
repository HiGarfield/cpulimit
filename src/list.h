/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifndef __LIST_H
#define __LIST_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>

/**
 * Structure representing a node in a doubly linked list.
 */
struct list_node
{
    /* Pointer to the content of the node */
    void *data;

    /* Pointer to the previous node in the list */
    struct list_node *previous;

    /* Pointer to the next node in the list */
    struct list_node *next;
};

/**
 * Structure representing a doubly linked list.
 */
struct list
{
    /* Pointer to the first node in the list */
    struct list_node *first;

    /* Pointer to the last node in the list */
    struct list_node *last;

    /* Size of the search key in bytes */
    size_t keysize;

    /* Count of elements in the list */
    size_t count;
};

/**
 * Initializes a list with a specified key size.
 *
 * @param l Pointer to the list to initialize.
 * @param keysize Size of the key used for comparisons.
 */
void init_list(struct list *l, size_t keysize);

/**
 * Adds a new element to the end of the list.
 *
 * @param l Pointer to the list to which the element will be added.
 * @param elem Pointer to the element to add.
 * @return Pointer to the newly created node containing the element.
 */
struct list_node *add_elem(struct list *l, void *elem);

/**
 * Deletes a specified node from the list.
 *
 * @param l Pointer to the list from which to delete the node.
 * @param node Pointer to the node to delete.
 */
void delete_node(struct list *l, struct list_node *node);

/**
 * Deletes a node from the list and frees its content.
 *
 * This function should only be used when the content is dynamically allocated.
 *
 * @param l Pointer to the list from which to delete the node.
 * @param node Pointer to the node to delete.
 */
void destroy_node(struct list *l, struct list_node *node);

/**
 * Checks whether the list is empty.
 *
 * @param l Pointer to the list to check.
 * @return Non-zero if the list is empty; zero otherwise.
 */
int is_empty_list(const struct list *l);

/**
 * Returns the count of elements in the list.
 *
 * @param l Pointer to the list.
 * @return Number of elements in the list.
 */
size_t get_list_count(const struct list *l);

/**
 * Returns the content of the first element in the list.
 *
 * @param l Pointer to the list.
 * @return Pointer to the content of the first node, or NULL if the list is empty.
 */
void *first_elem(const struct list *l);

/**
 * Returns the first node in the list.
 *
 * @param l Pointer to the list.
 * @return Pointer to the first node, or NULL if the list is empty.
 */
struct list_node *first_node(const struct list *l);

/**
 * Returns the content of the last element in the list.
 *
 * @param l Pointer to the list.
 * @return Pointer to the content of the last node, or NULL if the list is empty.
 */
void *last_elem(const struct list *l);

/**
 * Returns the last node in the list.
 *
 * @param l Pointer to the list.
 * @return Pointer to the last node, or NULL if the list is empty.
 */
struct list_node *last_node(const struct list *l);

/**
 * Searches for an element in the list by its content.
 *
 * Comparison is performed from the specified offset and for a specified length.
 * If offset=0, comparison starts from the address pointed to by data.
 * If length=0, the default keysize is used.
 *
 * @param l Pointer to the list to search.
 * @param elem Pointer to the element to locate.
 * @param offset Offset from which to start the comparison.
 * @param length Length of the comparison.
 * @return Pointer to the node if found; NULL if not found.
 */
struct list_node *xlocate_node(const struct list *l, const void *elem,
                               size_t offset, size_t length);

/**
 * Similar to xlocate_node(), but returns the content of the node.
 *
 * @param l Pointer to the list to search.
 * @param elem Pointer to the element to locate.
 * @param offset Offset from which to start the comparison.
 * @param length Length of the comparison.
 * @return Pointer to the content of the node if found; NULL if not found.
 */
void *xlocate_elem(const struct list *l, const void *elem,
                   size_t offset, size_t length);

/**
 * Locates a node in the list using default parameters (offset=0, length=0).
 *
 * @param l Pointer to the list to search.
 * @param elem Pointer to the element to locate.
 * @return Pointer to the node if found; NULL if not found.
 */
struct list_node *locate_node(const struct list *l, const void *elem);

/**
 * Similar to locate_node(), but returns the content of the node.
 *
 * @param l Pointer to the list to search.
 * @param elem Pointer to the element to locate.
 * @return Pointer to the content of the node if found; NULL if not found.
 */
void *locate_elem(const struct list *l, const void *elem);

/**
 * Deletes all elements in the list.
 *
 * @param l Pointer to the list to clear.
 */
void clear_list(struct list *l);

/**
 * Deletes every element in the list and frees the memory of all node data.
 *
 * @param l Pointer to the list to destroy.
 */
void destroy_list(struct list *l);

#endif
