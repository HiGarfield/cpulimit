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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define safe_free(p)     \
    do                   \
    {                    \
        if ((p) != NULL) \
        {                \
            free((p));   \
            (p) = NULL;  \
        }                \
    } while (0)

void init_list(struct list *l, int keysize)
{
    l->first = l->last = NULL;
    l->keysize = keysize;
    l->count = 0;
}

struct list_node *add_elem(struct list *l, void *elem)
{
    struct list_node *newnode = (struct list_node *)malloc(sizeof(struct list_node));
    if (newnode == NULL)
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
    if (l->count == 1)
    {
        l->first = l->last = NULL;
    }
    else if (node == l->first)
    {
        node->next->previous = NULL;
        l->first = node->next;
    }
    else if (node == l->last)
    {
        node->previous->next = NULL;
        l->last = node->previous;
    }
    else
    {
        node->previous->next = node->next;
        node->next->previous = node->previous;
    }
    l->count--;
    safe_free(node);
}

void destroy_node(struct list *l, struct list_node *node)
{
    safe_free(node->data);
    delete_node(l, node);
}

int is_empty_list(const struct list *l)
{
    return l->count == 0;
}

int get_list_count(const struct list *l)
{
    return l->count;
}

void *first_elem(struct list *l)
{
    return l->first->data;
}

struct list_node *first_node(const struct list *l)
{
    return l->first;
}

void *last_elem(struct list *l)
{
    return l->last->data;
}

struct list_node *last_node(const struct list *l)
{
    return l->last;
}

struct list_node *xlocate_node(struct list *l, const void *elem, int offset, int length)
{
    struct list_node *tmp;
    tmp = l->first;
    while (tmp != NULL)
    {
        if (!memcmp((char *)tmp->data + offset, elem, (size_t)(length == 0 ? l->keysize : length)))
            return (tmp);
        tmp = tmp->next;
    }
    return NULL;
}

struct list_node *locate_node(struct list *l, const void *elem)
{
    return (xlocate_node(l, elem, 0, 0));
}

void *xlocate_elem(struct list *l, const void *elem, int offset, int length)
{
    struct list_node *node = xlocate_node(l, elem, offset, length);
    return (node == NULL ? NULL : node->data);
}

void *locate_elem(struct list *l, const void *elem)
{
    return (xlocate_elem(l, elem, 0, 0));
}

void clear_list(struct list *l)
{
    while (l->first != NULL)
    {
        struct list_node *tmp;
        tmp = l->first;
        l->first = l->first->next;
        safe_free(tmp);
    }
    l->last = NULL;
    l->count = 0;
}

void destroy_list(struct list *l)
{
    while (l->first != NULL)
    {
        struct list_node *tmp;
        tmp = l->first;
        l->first = l->first->next;
        safe_free(tmp->data);
        safe_free(tmp);
    }
    l->last = NULL;
    l->count = 0;
}
