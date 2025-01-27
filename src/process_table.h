#ifndef __PROCESS_TABLE_H
#define __PROCESS_TABLE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "list.h"
#include "process_iterator.h"
#include <sys/types.h>

/**
 * Structure representing a process table.
 */
struct process_table
{
    /* Array of pointers to linked lists for storing processes */
    struct list **table;

    /* Size of the hash table for the process table */
    int hashsize;
};

/**
 * Initializes the process table with the given hash size.
 *
 * @param pt The process table to initialize
 * @param hashsize The size of the hash table
 */
void process_table_init(struct process_table *pt, int hashsize);

/**
 * Finds a process in the process table based on the given process.
 *
 * @param pt The process table to search in
 * @param p The process to find
 * @return A pointer to the found process or NULL if not found
 */
struct process *process_table_find(const struct process_table *pt, const struct process *p);

/**
 * Finds a process in the process table based on the given PID.
 *
 * @param pt The process table to search in
 * @param pid The PID of the process to find
 * @return A pointer to the found process or NULL if not found
 */
struct process *process_table_find_pid(const struct process_table *pt, pid_t pid);

/**
 * Adds a process to the process table.
 *
 * @param pt The process table to add the process to
 * @param p The process to add
 */
void process_table_add(struct process_table *pt, struct process *p);

/**
 * Deletes a process from the process table.
 *
 * @param pt The process table to delete the process from
 * @param p The process to delete
 * @return 0 if deletion is successful, 1 if process not found
 */
int process_table_del(struct process_table *pt, const struct process *p);

/**
 * Deletes a process from the process table based on the given PID.
 *
 * @param pt The process table to delete the process from
 * @param pid The PID of the process to delete
 * @return 0 if deletion is successful, 1 if process not found
 */
int process_table_del_pid(struct process_table *pt, pid_t pid);

/**
 * Destroys the process table and frees up the memory.
 *
 * @param pt The process table to destroy
 */
void process_table_destroy(struct process_table *pt);

#endif
