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

void init_list(struct list *lst) {
    if (lst == NULL) {
        return;
    }
    memset(lst, 0, sizeof(*lst));
}

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

void destroy_list_node(struct list *lst, struct list_node *node) {
    if (lst == NULL || node == NULL) {
        return;
    }
    free(node->data);
    delete_list_node(lst, node);
}

int is_empty_list(const struct list *lst) {
    return lst == NULL || lst->count == 0;
}

struct list_node *first_list_node(const struct list *lst) {
    return lst != NULL ? lst->first : NULL;
}

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
