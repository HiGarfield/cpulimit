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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "list.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_list(struct list *l, size_t keysize)
{
    if (l == NULL)
    {
        return;
    }
    l->first = l->last = NULL;
    l->keysize = keysize;
    l->count = 0;
}

struct list_node *add_elem(struct list *l, void *elem)
{
    struct list_node *newnode;
    if (l == NULL)
    {
        return NULL;
    }
    if ((newnode = (struct list_node *)malloc(sizeof(struct list_node))) == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the new list node\n");
        exit(EXIT_FAILURE);
    }
    newnode->data = elem;
    newnode->previous = l->last;
    newnode->next = NULL;
    if (l->count == 0)
    {
        l->first = l->last = newnode;
    }
    else
    {
        l->last->next = newnode;
        l->last = newnode;
    }
    l->count++;
    return newnode;
}

void delete_node(struct list *l, struct list_node *node)
{
    if (l == NULL || node == NULL || l->count == 0)
    {
        return;
    }

    if (node->previous != NULL)
    {
        node->previous->next = node->next;
    }
    else
    {
        l->first = node->next;
    }

    if (node->next != NULL)
    {
        node->next->previous = node->previous;
    }
    else
    {
        l->last = node->previous;
    }

    l->count--;
    free(node);
}

void destroy_node(struct list *l, struct list_node *node)
{
    if (node != NULL && node->data != NULL)
    {
        free(node->data);
    }
    delete_node(l, node);
}

int is_empty_list(const struct list *l)
{
    return l == NULL || l->count == 0;
}

size_t get_list_count(const struct list *l)
{
    return l != NULL ? l->count : 0;
}

struct list_node *first_node(const struct list *l)
{
    return l != NULL ? l->first : NULL;
}

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
static struct list_node *xlocate_node(const struct list *l, const void *elem,
                                      size_t offset, size_t length)
{
    struct list_node *cur;
    size_t cmp_len;

    if (l == NULL || elem == NULL)
    {
        return NULL;
    }

    cmp_len = (length != 0) ? length : l->keysize;
    if (cmp_len == 0)
    {
        return NULL;
    }

    for (cur = l->first; cur != NULL; cur = cur->next)
    {
        if (memcmp((const char *)cur->data + offset, elem, cmp_len) == 0)
        {
            return cur;
        }
    }

    return NULL;
}

struct list_node *locate_node(const struct list *l, const void *elem)
{
    return xlocate_node(l, elem, 0, 0);
}

/**
 * Similar to xlocate_node(), but returns the content of the node.
 *
 * @param l Pointer to the list to search.
 * @param elem Pointer to the element to locate.
 * @param offset Offset from which to start the comparison.
 * @param length Length of the comparison.
 * @return Pointer to the content of the node if found; NULL if not found.
 */
static void *xlocate_elem(const struct list *l, const void *elem,
                          size_t offset, size_t length)
{
    struct list_node *node = xlocate_node(l, elem, offset, length);
    return node != NULL ? node->data : NULL;
}

void *locate_elem(const struct list *l, const void *elem)
{
    return xlocate_elem(l, elem, 0, 0);
}

static void clear_all_list_nodes(struct list *l, int free_data)
{
    struct list_node *current, *next;
    if (l == NULL || l->count == 0)
    {
        return;
    }
    for (current = l->first; current != NULL; current = next)
    {
        next = current->next;
        if (free_data && current->data != NULL)
        {
            free(current->data);
        }
        free(current);
    }
    l->first = l->last = NULL;
    l->count = 0;
}

void clear_list(struct list *l)
{
    clear_all_list_nodes(l, 0);
}

void destroy_list(struct list *l)
{
    clear_all_list_nodes(l, 1);
}
