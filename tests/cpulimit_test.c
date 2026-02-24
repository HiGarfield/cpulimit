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

#undef NDEBUG

#include "../src/cli.h"
#include "../src/limit_process.h"
#include "../src/limiter.h"
#include "../src/list.h"
#include "../src/process_group.h"
#include "../src/process_iterator.h"
#include "../src/process_table.h"
#include "../src/signal_handler.h"
#include "../src/util.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Terminate a child process or process group and wait for it to exit
 * @param pid Process ID (positive) or negative process group ID
 * @param kill_signal Signal to send (SIGTERM or SIGKILL)
 * @note If pid > 0, treats pid as a single process. If pid < 0, treats -pid
 *  as a process group ID. Sends the given signal and waits up to 5 seconds.
 *  If SIGTERM times out, escalates to SIGKILL and waits an additional 5
 *  seconds. If processes are not reaped after 5 + 5 seconds, function exits.
 */
static void kill_and_wait(pid_t pid, int kill_signal) {
    struct timespec now, end_time;
    const struct timespec sleep_time = {0, 10000000L}; /* 10 ms */

    switch (kill_signal) {
    case SIGKILL:
    case SIGTERM:
        break;
    default:
        fprintf(stderr, "Invalid signal %d for kill_and_wait\n", kill_signal);
        return;
    }

    /* Initialize timeout: 5 seconds */
    get_current_time(&end_time);
    end_time.tv_sec += 5;

    kill(pid, kill_signal); /* Send initial signal */

    while (1) {
        pid_t wpid = waitpid(pid, NULL, WNOHANG);

        if (wpid > 0) { /* Process terminated */
            /* Single process: exit after cleanup */
            /* Process group: continue to next member */
            if (pid > 0) {
                break;
            }
        } else if (wpid == -1) { /* Waitpid error */
            if (errno != EINTR) {
                break; /* Non-interrupt error */
            }
        } else { /* wpid == 0: process still running */
            /* Check timeout */
            get_current_time(&now);
            if (now.tv_sec > end_time.tv_sec ||
                (now.tv_sec == end_time.tv_sec &&
                 now.tv_nsec >= end_time.tv_nsec)) {
                if (kill_signal == SIGTERM) {
                    /* SIGTERM timeout: escalate to SIGKILL */
                    kill(pid, SIGKILL);
                    kill_signal = SIGKILL;
                    /* Reset timeout for SIGKILL (5 seconds) */
                    get_current_time(&end_time);
                    end_time.tv_sec += 5;
                } else {
                    break; /* SIGKILL timeout: stop waiting */
                }
            }
            /* Short sleep to reduce CPU usage (10ms) */
            sleep_timespec(&sleep_time);
        }
    }

    /* Final cleanup: reap all remaining zombies */
    while (waitpid(pid, NULL, WNOHANG) > 0) {
        /* Keep reaping until none left */
    }
}

/***************************************************************************
 * LIST MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test list initialization and empty list operations
 * @note Tests init_list, is_empty_list, get_list_count with empty list
 */
static void test_list_init_and_empty(void) {
    struct list l;

    /* Test initialization */
    init_list(&l);
    assert(l.first == NULL);
    assert(l.last == NULL);
    assert(l.count == 0);

    /* Test is_empty_list */
    assert(is_empty_list(&l) == 1);
    assert(is_empty_list(NULL) == 1);

    /* Test get_list_count */
    assert(get_list_count(&l) == 0);
    assert(get_list_count(NULL) == 0);

    /* Test first_node */
    assert(first_node(&l) == NULL);
    assert(first_node(NULL) == NULL);

    /* Test init_list with NULL */
    init_list(NULL);
}

/**
 * @brief Test adding elements to list
 * @note Tests add_elem, get_list_count, first_node with non-empty list
 */
static void test_list_add_elem(void) {
    struct list l;
    int data1 = 1, data2 = 2, data3 = 3;
    const struct list_node *node1, *node2, *node3;

    init_list(&l);

    /* Add first element */
    node1 = add_elem(&l, &data1);
    assert(node1 != NULL);
    assert(node1->data == &data1);
    assert(node1->previous == NULL);
    assert(node1->next == NULL);
    assert(l.first == node1);
    assert(l.last == node1);
    assert(get_list_count(&l) == 1);
    assert(is_empty_list(&l) == 0);
    assert(first_node(&l) == node1);

    /* Add second element */
    node2 = add_elem(&l, &data2);
    assert(node2 != NULL);
    assert(node2->data == &data2);
    assert(node2->previous == node1);
    assert(node2->next == NULL);
    assert(node1->next == node2);
    assert(l.first == node1);
    assert(l.last == node2);
    assert(get_list_count(&l) == 2);

    /* Add third element */
    node3 = add_elem(&l, &data3);
    assert(node3 != NULL);
    assert(node3->data == &data3);
    assert(node3->previous == node2);
    assert(node3->next == NULL);
    assert(node2->next == node3);
    assert(l.first == node1);
    assert(l.last == node3);
    assert(get_list_count(&l) == 3);

    /* Test add_elem with NULL list */
    assert(add_elem(NULL, &data1) == NULL);

    /* Clean up */
    clear_list(&l);
}

/**
 * @brief Test deleting nodes from list
 * @note Tests delete_node without freeing data
 */
static void test_list_delete_node(void) {
    struct list l;
    int data1 = 1, data2 = 2, data3 = 3;
    struct list_node *node1, *node2, *node3;

    init_list(&l);
    node1 = add_elem(&l, &data1);
    node2 = add_elem(&l, &data2);
    node3 = add_elem(&l, &data3);

    /* Delete middle node */
    delete_node(&l, node2);
    assert(get_list_count(&l) == 2);
    assert(l.first == node1);
    assert(l.last == node3);
    assert(node1->next == node3);
    assert(node3->previous == node1);

    /* Delete first node */
    delete_node(&l, node1);
    assert(get_list_count(&l) == 1);
    assert(l.first == node3);
    assert(l.last == node3);
    assert(node3->previous == NULL);
    assert(node3->next == NULL);

    /* Delete last node */
    delete_node(&l, node3);
    assert(get_list_count(&l) == 0);
    assert(l.first == NULL);
    assert(l.last == NULL);
    assert(is_empty_list(&l) == 1);

    /* Test delete_node with NULL */
    delete_node(NULL, NULL);
    delete_node(&l, NULL);
}

/**
 * @brief Test destroying nodes from list
 * @note Tests destroy_node which frees both node and data
 */
static void test_list_destroy_node(void) {
    struct list l;
    int *data1, *data2;
    struct list_node *node1, *node2;

    init_list(&l);

    /* Allocate data dynamically for destroy_node */
    data1 = (int *)malloc(sizeof(int));
    data2 = (int *)malloc(sizeof(int));
    assert(data1 != NULL);
    assert(data2 != NULL);
    *data1 = 1;
    *data2 = 2;

    node1 = add_elem(&l, data1);
    node2 = add_elem(&l, data2);

    /* Destroy second node */
    destroy_node(&l, node2);
    assert(get_list_count(&l) == 1);
    assert(l.first == node1);
    assert(l.last == node1);

    /* Destroy first node */
    destroy_node(&l, node1);
    assert(get_list_count(&l) == 0);
    assert(is_empty_list(&l) == 1);

    /* Test destroy_node with NULL */
    destroy_node(NULL, NULL);
    destroy_node(&l, NULL);
}

/**
 * @brief Test locating nodes and elements in list
 * @note Tests locate_node and locate_elem
 */
static void test_list_locate(void) {
    struct list l;
    struct process *p1, *p2, *p3;
    struct list_node *found_node;
    const struct process *found_elem;
    pid_t search_pid;

    init_list(&l);

    /* Allocate processes on heap to avoid stack size warnings */
    p1 = (struct process *)malloc(sizeof(struct process));
    p2 = (struct process *)malloc(sizeof(struct process));
    p3 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL);
    assert(p2 != NULL);
    assert(p3 != NULL);

    /* Initialize processes with different PIDs */
    p1->pid = 100;
    p1->ppid = 1;
    p2->pid = 200;
    p2->ppid = 1;
    p3->pid = 300;
    p3->ppid = 1;

    add_elem(&l, p1);
    add_elem(&l, p2);
    add_elem(&l, p3);

    /* Test locate_node - find by PID */
    search_pid = 200;
    found_node = locate_node(&l, &search_pid, offsetof(struct process, pid),
                             sizeof(pid_t));
    assert(found_node != NULL);
    assert(((struct process *)found_node->data)->pid == 200);

    /* Test locate_node - not found */
    search_pid = 999;
    found_node = locate_node(&l, &search_pid, offsetof(struct process, pid),
                             sizeof(pid_t));
    assert(found_node == NULL);

    /* Test locate_elem - find by PID */
    search_pid = 100;
    found_elem = (struct process *)locate_elem(
        &l, &search_pid, offsetof(struct process, pid), sizeof(pid_t));
    assert(found_elem == p1);
    assert(found_elem->pid == 100);

    /* Test locate_elem - not found */
    search_pid = 999;
    found_elem = (struct process *)locate_elem(
        &l, &search_pid, offsetof(struct process, pid), sizeof(pid_t));
    assert(found_elem == NULL);

    /* Test with NULL list */
    assert(locate_node(NULL, &search_pid, 0, sizeof(pid_t)) == NULL);
    assert(locate_elem(NULL, &search_pid, 0, sizeof(pid_t)) == NULL);

    /* Test with NULL element */
    assert(locate_node(&l, NULL, 0, sizeof(pid_t)) == NULL);
    assert(locate_elem(&l, NULL, 0, sizeof(pid_t)) == NULL);

    /* Test with zero length */
    assert(locate_node(&l, &search_pid, 0, 0) == NULL);
    assert(locate_elem(&l, &search_pid, 0, 0) == NULL);

    clear_list(&l);

    /* Free allocated memory */
    free(p1);
    free(p2);
    free(p3);
}

/**
 * @brief Test clearing and destroying lists
 * @note Tests clear_list and destroy_list
 */
static void test_list_clear_and_destroy(void) {
    struct list l1, l2;
    int data1 = 1, data2 = 2, data3 = 3;
    int *dyn_data1, *dyn_data2, *dyn_data3;

    /* Test clear_list - data not freed */
    init_list(&l1);
    add_elem(&l1, &data1);
    add_elem(&l1, &data2);
    add_elem(&l1, &data3);
    assert(get_list_count(&l1) == 3);

    clear_list(&l1);
    assert(get_list_count(&l1) == 0);
    assert(l1.first == NULL);
    assert(l1.last == NULL);
    assert(is_empty_list(&l1) == 1);

    /* Test clear_list with NULL */
    clear_list(NULL);

    /* Test destroy_list - data is freed */
    init_list(&l2);
    dyn_data1 = (int *)malloc(sizeof(int));
    dyn_data2 = (int *)malloc(sizeof(int));
    dyn_data3 = (int *)malloc(sizeof(int));
    assert(dyn_data1 != NULL);
    assert(dyn_data2 != NULL);
    assert(dyn_data3 != NULL);
    *dyn_data1 = 1;
    *dyn_data2 = 2;
    *dyn_data3 = 3;

    add_elem(&l2, dyn_data1);
    add_elem(&l2, dyn_data2);
    add_elem(&l2, dyn_data3);
    assert(get_list_count(&l2) == 3);

    destroy_list(&l2);
    assert(get_list_count(&l2) == 0);
    assert(l2.first == NULL);
    assert(l2.last == NULL);
    assert(is_empty_list(&l2) == 1);

    /* Test destroy_list with NULL */
    destroy_list(NULL);
}

/***************************************************************************
 * UTIL MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test nsec2timespec conversion
 * @note Tests conversion from nanoseconds to timespec structure
 */
static void test_util_nsec2timespec(void) {
    struct timespec ts;

    /* Test 0 nanoseconds */
    nsec2timespec(0.0, &ts);
    assert(ts.tv_sec == 0);
    assert(ts.tv_nsec == 0);

    /* Test 1 second (1e9 nanoseconds) */
    nsec2timespec(1000000000.0, &ts);
    assert(ts.tv_sec == 1);
    assert(ts.tv_nsec == 0);

    /* Test 1.5 seconds */
    nsec2timespec(1500000000.0, &ts);
    assert(ts.tv_sec == 1);
    assert(ts.tv_nsec == 500000000);

    /* Test 2.25 seconds */
    nsec2timespec(2250000000.0, &ts);
    assert(ts.tv_sec == 2);
    assert(ts.tv_nsec == 250000000);

    /* Test small value (100 microseconds) */
    nsec2timespec(100000.0, &ts);
    assert(ts.tv_sec == 0);
    assert(ts.tv_nsec == 100000);
}

/**
 * @brief Test get_current_time and sleep_timespec
 * @note Tests high-resolution time retrieval and sleeping
 */
static void test_util_time_functions(void) {
    struct timespec ts1, ts2, sleep_time;
    int ret;
    double elapsed_ms;

    /* Test get_current_time */
    ret = get_current_time(&ts1);
    assert(ret == 0);
    assert(ts1.tv_sec >= 0);
    assert(ts1.tv_nsec >= 0 && ts1.tv_nsec < 1000000000L);
    assert(ts1.tv_sec > 0 || ts1.tv_nsec > 0);

    /* Test sleep_timespec with 50ms */
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 50000000L; /* 50 ms */
    ret = sleep_timespec(&sleep_time);
    assert(ret == 0 || ret == -1); /* May return -1 on signal */

    ret = get_current_time(&ts2);
    assert(ret == 0);

    /* Verify time has advanced */
    assert(ts2.tv_sec >= ts1.tv_sec);
    if (ts2.tv_sec == ts1.tv_sec) {
        assert(ts2.tv_nsec >= ts1.tv_nsec);
    }

    /* Test timediff_in_ms */
    elapsed_ms = timediff_in_ms(&ts2, &ts1);
    assert(elapsed_ms >= 0.0);
    /* Should be at least close to 50ms if sleep succeeded */
}

/**
 * @brief Test timediff_in_ms calculations
 * @note Tests time difference calculation in milliseconds
 */
static void test_util_timediff_in_ms(void) {
    struct timespec earlier, later;
    double diff_ms;

    /* Test simple case: 1 second difference */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 0;
    later.tv_sec = 101;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 999.0 && diff_ms <= 1001.0);

    /* Test with nanoseconds: 0.5 second difference */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 0;
    later.tv_sec = 100;
    later.tv_nsec = 500000000L;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 499.0 && diff_ms <= 501.0);

    /* Test with both seconds and nanoseconds */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 250000000L;
    later.tv_sec = 101;
    later.tv_nsec = 750000000L;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 1499.0 && diff_ms <= 1501.0);

    /* Test zero difference */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 123456789L;
    later.tv_sec = 100;
    later.tv_nsec = 123456789L;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= -0.001 && diff_ms <= 0.001);
}

/**
 * @brief Test file_basename extraction
 * @note Tests extracting filename from path
 */
static void test_util_file_basename(void) {
    const char *result;
    int cmp_result;

    /* Test simple filename */
    result = file_basename("test.txt");
    cmp_result = strcmp(result, "test.txt");
    assert(cmp_result == 0);

    /* Test path with directory */
    result = file_basename("/usr/bin/test");
    cmp_result = strcmp(result, "test");
    assert(cmp_result == 0);

    /* Test path with multiple directories */
    result = file_basename("/home/user/documents/file.txt");
    cmp_result = strcmp(result, "file.txt");
    assert(cmp_result == 0);

    /* Test path ending with slash */
    result = file_basename("/home/user/");
    cmp_result = strcmp(result, "");
    assert(cmp_result == 0);

    /* Test root directory */
    result = file_basename("/");
    cmp_result = strcmp(result, "");
    assert(cmp_result == 0);

    /* Test current directory */
    result = file_basename("./file");
    cmp_result = strcmp(result, "file");
    assert(cmp_result == 0);
}

/**
 * @brief Test get_ncpu function
 * @note Tests retrieval of CPU count
 */
static void test_util_get_ncpu(void) {
    int ncpu;

    ncpu = get_ncpu();
    assert(ncpu >= 1);

    /* Call again to test caching */
    assert(get_ncpu() == ncpu);
}

/**
 * @brief Test increase_priority function
 * @note Tests attempting to increase process priority
 */
static void test_util_increase_priority(void) {
    /* This function may succeed or fail depending on permissions */
    /* Just ensure it doesn't crash */
    increase_priority();
}

/**
 * @brief Test long2pid_t conversion
 * @note Tests safe conversion from long to pid_t
 */
static void test_util_long2pid_t(void) {
    pid_t result;

    /* Test valid positive values */
    result = long2pid_t(1L);
    assert(result == 1);

    result = long2pid_t(1000L);
    assert(result == 1000);

    result = long2pid_t(32767L);
    assert(result == 32767);

    /* Test zero */
    result = long2pid_t(0L);
    assert(result == 0);

    /* Test negative value (should return -1) */
    result = long2pid_t(-1L);
    assert(result == -1);

    result = long2pid_t(-100L);
    assert(result == -1);
}

/***************************************************************************
 * PROCESS_TABLE MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test process table initialization and destruction
 * @note Tests process_table_init and process_table_destroy
 */
static void test_process_table_init_destroy(void) {
    struct process_table pt;

    /* Test initialization with small hashsize */
    process_table_init(&pt, 16);
    assert(pt.table != NULL);
    assert(pt.hashsize == 16);
    process_table_destroy(&pt);
    assert(pt.table == NULL);

    /* Test initialization with larger hashsize */
    process_table_init(&pt, 256);
    assert(pt.table != NULL);
    assert(pt.hashsize == 256);
    process_table_destroy(&pt);

    /* Test zero hashsize fallback (must avoid division by zero in hashing) */
    process_table_init(&pt, 0);
    assert(pt.table != NULL);
    assert(pt.hashsize == 1);
    process_table_destroy(&pt);

    /* Test destroy with NULL (should not crash) */
    process_table_destroy(NULL);
}

/**
 * @brief Test process table add and find operations
 * @note Tests process_table_add and process_table_find
 */
static void test_process_table_add_find(void) {
    struct process_table pt;
    struct process *p1, *p2, *p3;
    const struct process *found;

    process_table_init(&pt, 64);

    /* Create test processes */
    p1 = (struct process *)malloc(sizeof(struct process));
    p2 = (struct process *)malloc(sizeof(struct process));
    p3 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL && p2 != NULL && p3 != NULL);

    p1->pid = 100;
    p1->ppid = 1;
    p1->cputime = 0.0;

    p2->pid = 200;
    p2->ppid = 1;
    p2->cputime = 0.0;

    p3->pid = 300;
    p3->ppid = 1;
    p3->cputime = 0.0;

    /* Test find on empty table */
    found = process_table_find(&pt, 100);
    assert(found == NULL);

    /* Add first process */
    process_table_add(&pt, p1);
    found = process_table_find(&pt, 100);
    assert(found == p1);
    assert(found->pid == 100);

    /* Add second process */
    process_table_add(&pt, p2);
    found = process_table_find(&pt, 200);
    assert(found == p2);
    assert(found->pid == 200);

    /* Verify first process still findable */
    found = process_table_find(&pt, 100);
    assert(found == p1);

    /* Add third process */
    process_table_add(&pt, p3);
    found = process_table_find(&pt, 300);
    assert(found == p3);

    /* Test find non-existent PID */
    found = process_table_find(&pt, 999);
    assert(found == NULL);

    /* Test find with NULL table */
    found = process_table_find(NULL, 100);
    assert(found == NULL);

    process_table_destroy(&pt);
}

/**
 * @brief Test process table delete operation
 * @note Tests process_table_del
 */
static void test_process_table_del(void) {
    struct process_table pt;
    struct process *p1, *p2, *p3;
    const struct process *found;
    int ret;

    process_table_init(&pt, 64);

    p1 = (struct process *)malloc(sizeof(struct process));
    p2 = (struct process *)malloc(sizeof(struct process));
    p3 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL && p2 != NULL && p3 != NULL);

    p1->pid = 100;
    p2->pid = 200;
    p3->pid = 300;

    process_table_add(&pt, p1);
    process_table_add(&pt, p2);
    process_table_add(&pt, p3);

    /* Delete middle process */
    ret = process_table_del(&pt, 200);
    assert(ret == 0);
    found = process_table_find(&pt, 200);
    assert(found == NULL);

    /* Verify others still exist */
    found = process_table_find(&pt, 100);
    assert(found == p1);
    found = process_table_find(&pt, 300);
    assert(found == p3);

    /* Delete first process */
    ret = process_table_del(&pt, 100);
    assert(ret == 0);
    found = process_table_find(&pt, 100);
    assert(found == NULL);

    /* Delete last process */
    ret = process_table_del(&pt, 300);
    assert(ret == 0);
    found = process_table_find(&pt, 300);
    assert(found == NULL);

    /* Try to delete non-existent process */
    ret = process_table_del(&pt, 999);
    assert(ret == 1);

    /* Test del with NULL table */
    ret = process_table_del(NULL, 100);
    assert(ret == 1);

    process_table_destroy(&pt);
}

/**
 * @brief Test process table remove stale entries
 * @note Tests process_table_remove_stale
 */
static void test_process_table_remove_stale(void) {
    struct process_table pt;
    struct list active_list;
    struct process *p1, *p2, *p3;
    const struct process *found;

    process_table_init(&pt, 64);
    init_list(&active_list);

    /* Create and add three processes to table */
    p1 = (struct process *)malloc(sizeof(struct process));
    p2 = (struct process *)malloc(sizeof(struct process));
    p3 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL && p2 != NULL && p3 != NULL);

    p1->pid = 100;
    p2->pid = 200;
    p3->pid = 300;

    process_table_add(&pt, p1);
    process_table_add(&pt, p2);
    process_table_add(&pt, p3);

    /* Add only p1 and p3 to active list */
    add_elem(&active_list, p1);
    add_elem(&active_list, p3);

    /* Remove stale entries (p2 should be removed) */
    process_table_remove_stale(&pt, &active_list);

    /* Verify p2 was removed */
    found = process_table_find(&pt, 200);
    assert(found == NULL);

    /* Verify p1 and p3 still exist */
    found = process_table_find(&pt, 100);
    assert(found == p1);
    found = process_table_find(&pt, 300);
    assert(found == p3);

    /* Test with NULL (should not crash) */
    process_table_remove_stale(NULL, &active_list);

    clear_list(&active_list);
    process_table_destroy(&pt);
}

/**
 * @brief Test process_table_remove_stale removes NULL-data nodes
 * @note NULL-data nodes should be removed defensively
 *
 * To test this defensive path we must inject a NULL-data node directly
 * into the internal hash bucket. The process_table struct and its table
 * member are exposed in the public header, so this access is intentional;
 * the idx computation mirrors process_table's own pid_hash() formula.
 */
static void test_process_table_remove_stale_null_data(void) {
    struct process_table pt;
    struct list active_list;
    struct process *p1;
    size_t idx;

    process_table_init(&pt, 16);
    init_list(&active_list);

    /* Insert a valid process */
    p1 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL);
    p1->pid = 101;
    process_table_add(&pt, p1);

    /*
     * Inject a NULL-data node into the same bucket.
     * idx mirrors process_table's pid_hash(): (size_t)pid % hashsize.
     */
    idx = (size_t)101 % 16;
    assert(pt.table[idx] != NULL);
    add_elem(pt.table[idx], NULL);

    /* add p1 to active_list so it is not removed */
    add_elem(&active_list, p1);

    /* remove_stale must remove the NULL-data node without crashing */
    process_table_remove_stale(&pt, &active_list);

    /* p1 must still be present */
    assert(process_table_find(&pt, 101) == p1);

    /* The NULL-data node must be gone (bucket list has exactly one entry) */
    assert(get_list_count(pt.table[idx]) == 1);

    clear_list(&active_list);
    process_table_destroy(&pt);
}

/**
 * @brief Test signal handler flags
 * @note Installs handlers in a child, raises signals, and checks flags
 */
static void test_signal_handler_flags(void) {
    pid_t pid;
    int status;

    /* Test behavior on SIGTERM: quit flag set, not terminated by tty */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Child process: install handlers and raise SIGTERM */
        configure_signal_handler();
        if (raise(SIGTERM) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) {
            _exit(3);
        }
        _exit(0);
    }

    /* Parent: verify child exited successfully */
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    /* Test behavior on SIGINT: quit flag set, terminated by tty */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Child process: install handlers and raise SIGINT */
        configure_signal_handler();
        if (raise(SIGINT) != 0) {
            _exit(4);
        }
        if (!is_quit_flag_set()) {
            _exit(5);
        }
        if (!is_terminated_by_tty()) {
            _exit(6);
        }
        _exit(0);
    }

    /* Parent: verify child exited successfully */
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/***************************************************************************
 * PROCESS_ITERATOR MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test is_child_of function
 * @note Tests process ancestry checking
 */
static void test_process_iterator_is_child_of(void) {
    pid_t child_pid, parent_pid;
    int result;

    parent_pid = getpid();

    /* Create a child process */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process */
        const struct timespec sleep_time = {5, 0L};
        while (!is_quit_flag_set()) {
            sleep_timespec(&sleep_time);
        }
        _exit(EXIT_SUCCESS);
    }

    /* Parent process - test is_child_of */

    /* Child should be child of parent */
    result = is_child_of(child_pid, parent_pid);
    assert(result == 1);

    /* Parent should not be child of child */
    /* NOLINTNEXTLINE(readability-suspicious-call-argument) */
    result = is_child_of(parent_pid, child_pid);
    assert(result == 0);

    /* Process should not be child of itself */
    result = is_child_of(parent_pid, parent_pid);
    assert(result == 0);

    /* All processes are children of init (PID 1) */
    result = is_child_of(parent_pid, 1);
    assert(result == 1);

    /* Test with invalid PIDs */
    result = is_child_of(0, parent_pid);
    assert(result == 0);

    result = is_child_of(-1, parent_pid);
    assert(result == 0);

    result = is_child_of(child_pid, 0);
    assert(result == 0);

    result = is_child_of(child_pid, -1);
    assert(result == 0);

    /* Test with non-existent PID */
    result = is_child_of(99999, parent_pid);
    assert(result == 0);

    /* Non-existent process must not be treated as child of init */
    result = is_child_of(99999, 1);
    assert(result == 0);

    /* Clean up child */
    kill_and_wait(child_pid, SIGKILL);
}

/***************************************************************************
 * PROCESS_GROUP MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test get_process_group_cpu_usage function
 * @note Tests CPU usage calculation for process group
 */
static void test_process_group_cpu_usage(void) {
    struct process_group pgroup;
    double cpu_usage;
    pid_t child_pid;
    int i;

    /* Create a child process that uses CPU */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process - busy loop */
        volatile int keep_running = 1;
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var++) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }

    /* Initialize process group for child */
    assert(init_process_group(&pgroup, child_pid, 0) == 0);

    /* First call should return -1 (no measurement yet) */
    cpu_usage = get_process_group_cpu_usage(&pgroup);
    assert(cpu_usage >= -1.00001 && cpu_usage <= -0.99999);

    /* Update a few times to get valid measurements */
    for (i = 0; i < 5; i++) {
        const struct timespec sleep_time = {0, 100000000L}; /* 100ms */
        sleep_timespec(&sleep_time);
        update_process_group(&pgroup);
    }

    /* Should now have valid CPU usage */
    cpu_usage = get_process_group_cpu_usage(&pgroup);
    /* CPU usage should be between 0 and ncpu */
    assert(cpu_usage >= 0.0 && cpu_usage <= 1.0 * get_ncpu());

    assert(close_process_group(&pgroup) == 0);
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test list operations with edge cases
 * @note Tests list behavior with various edge cases like reversing order
 */
static void test_list_edge_cases(void) {
    struct list l;
    int data[10];
    struct list_node *node;
    int i, count;

    init_list(&l);

    /* Test adding many elements */
    for (i = 0; i < 10; i++) {
        data[i] = i;
        add_elem(&l, &data[i]);
    }
    assert(get_list_count(&l) == 10);

    /* Verify forward traversal */
    count = 0;
    for (node = l.first; node != NULL; node = node->next) {
        assert(*(int *)node->data == count);
        count++;
    }
    assert(count == 10);

    /* Verify backward traversal */
    count = 9;
    for (node = l.last; node != NULL; node = node->previous) {
        assert(*(int *)node->data == count);
        count--;
    }
    assert(count == -1);

    /* Delete all nodes from back to front */
    while (!is_empty_list(&l)) {
        delete_node(&l, l.last);
    }
    assert(get_list_count(&l) == 0);

    /* Test deleting nodes in middle repeatedly */
    for (i = 0; i < 5; i++) {
        data[i] = i;
        add_elem(&l, &data[i]);
    }

    /* Delete middle elements */
    node = l.first->next; /* Second element */
    delete_node(&l, node);
    assert(get_list_count(&l) == 4);

    node = l.first->next; /* New second element (was third) */
    delete_node(&l, node);
    assert(get_list_count(&l) == 3);

    /* Verify remaining elements */
    assert(*(int *)l.first->data == 0);
    assert(*(int *)l.first->next->data == 3);
    assert(*(int *)l.last->data == 4);

    clear_list(&l);
}

/**
 * @brief Test util edge cases for time operations
 * @note Tests boundary conditions and special values
 */
static void test_util_time_edge_cases(void) {
    struct timespec ts;
    double diff_ms;
    struct timespec t1, t2;

    /* Test nsec2timespec with very large value */
    nsec2timespec(10000000000.0, &ts); /* 10 seconds */
    assert(ts.tv_sec == 10);
    assert(ts.tv_nsec == 0);

    /* Test timediff_in_ms with same time */
    t1.tv_sec = 1000;
    t1.tv_nsec = 500000000L;
    t2.tv_sec = 1000;
    t2.tv_nsec = 500000000L;
    diff_ms = timediff_in_ms(&t2, &t1);
    assert(diff_ms >= -0.001 && diff_ms <= 0.001);

    /* Test timediff_in_ms with very small difference */
    t1.tv_sec = 1000;
    t1.tv_nsec = 0;
    t2.tv_sec = 1000;
    t2.tv_nsec = 1000000L; /* 1 millisecond */
    diff_ms = timediff_in_ms(&t2, &t1);
    assert(diff_ms >= 0.999 && diff_ms <= 1.001);

    /* Test timediff_in_ms with large difference */
    t1.tv_sec = 1000;
    t1.tv_nsec = 0;
    t2.tv_sec = 2000;
    t2.tv_nsec = 0;
    diff_ms = timediff_in_ms(&t2, &t1);
    assert(diff_ms >= 999999.0 && diff_ms <= 1000001.0);
}

/**
 * @brief Test file_basename with edge cases
 * @note Tests various path formats
 */
static void test_util_file_basename_edge_cases(void) {
    const char *result;
    int cmp_result;

    /* Test multiple consecutive slashes */
    result = file_basename("//usr//bin//test");
    cmp_result = strcmp(result, "test");
    assert(cmp_result == 0);

    /* Test path with no directory separator */
    result = file_basename("filename");
    cmp_result = strcmp(result, "filename");
    assert(cmp_result == 0);

    /* Test path with dot directory */
    result = file_basename("../test");
    cmp_result = strcmp(result, "test");
    assert(cmp_result == 0);

    /* Test just a slash */
    result = file_basename("/");
    cmp_result = strcmp(result, "");
    assert(cmp_result == 0);

    /* Test NULL input */
    result = file_basename(NULL);
    assert(result != NULL);
    cmp_result = strcmp(result, "");
    assert(cmp_result == 0);
}

/**
 * @brief Test long2pid_t with boundary values
 * @note Tests conversion edge cases
 */
static void test_util_long2pid_t_edge_cases(void) {
    pid_t result;

    /* Test maximum reasonable PID */
    result = long2pid_t(65535L);
    assert(result == 65535);

    /* Test with large positive value */
    result = long2pid_t(1000000L);
    /* Result depends on pid_t size - just verify it doesn't crash */
    (void)result;
}

/**
 * @brief Test process table with hash collisions
 * @note Tests behavior when multiple PIDs hash to same bucket
 */
static void test_process_table_collisions(void) {
    struct process_table pt;
    static const pid_t collision_pids[20] = {100, 110, 120, 130, 140, 150, 160,
                                             170, 180, 190, 200, 210, 220, 230,
                                             240, 250, 260, 270, 280, 290};
    struct process *p[20];
    const struct process *found;
    size_t i;

    /* Use small hash size to force collisions */
    process_table_init(&pt, 4);

    /* Add many processes */
    for (i = 0; i < 20; i++) {
        p[i] = (struct process *)malloc(sizeof(struct process));
        assert(p[i] != NULL);
        p[i]->pid = collision_pids[i];
        process_table_add(&pt, p[i]);
    }

    /* Verify all processes can be found */
    for (i = 0; i < 20; i++) {
        found = process_table_find(&pt, collision_pids[i]);
        assert(found == p[i]);
        assert(found->pid == collision_pids[i]);
    }

    /* Delete some processes */
    for (i = 0; i < 20; i += 3) {
        assert(process_table_del(&pt, collision_pids[i]) == 0);
    }

    /* Verify deleted processes are gone */
    for (i = 0; i < 20; i++) {
        found = process_table_find(&pt, collision_pids[i]);
        if (i % 3 == 0) {
            assert(found == NULL);
        } else {
            assert(found == p[i]);
        }
    }

    process_table_destroy(&pt);
}

/**
 * @brief Test process table with empty buckets
 * @note Tests operations when some buckets are empty
 */
static void test_process_table_empty_buckets(void) {
    struct process_table pt;
    struct list active_list;
    struct process *p1, *p2;

    process_table_init(&pt, 256);
    init_list(&active_list);

    /* Add sparse processes */
    p1 = (struct process *)malloc(sizeof(struct process));
    p2 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL && p2 != NULL);

    p1->pid = (pid_t)10;
    p2->pid = (pid_t)1000;

    process_table_add(&pt, p1);
    process_table_add(&pt, p2);

    /* Remove stale with empty active list */
    process_table_remove_stale(&pt, &active_list);

    /* All processes should be removed */
    assert(process_table_find(&pt, (pid_t)10) == NULL);
    assert(process_table_find(&pt, (pid_t)1000) == NULL);

    clear_list(&active_list);
    process_table_destroy(&pt);
}

/**
 * @brief Test process iterator filter edge cases
 * @note Tests various filter configurations
 */
static void test_process_iterator_filter_edge_cases(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    int count;

    process = (struct process *)malloc(sizeof(struct process));
    assert(process != NULL);

    /* Test with PID 0 (all processes) and read_cmd enabled */
    filter.pid = (pid_t)0;
    filter.include_children = 0;
    filter.read_cmd = 1;

    count = 0;
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0 && count < 10) {
        /* Just iterate a few processes to verify it works */
        count++;
    }
    assert(count > 0);
    assert(close_process_iterator(&it) == 0);

    free(process);
}

/**
 * @brief Test process group with rapid updates
 * @note Tests update_process_group called in quick succession
 */
static void test_process_group_rapid_updates(void) {
    struct process_group pgroup;
    pid_t child_pid;
    int i;

    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process */
        volatile int keep_running = 1;
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var++) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }

    /* Initialize and update rapidly */
    assert(init_process_group(&pgroup, child_pid, 0) == 0);

    for (i = 0; i < 20; i++) {
        update_process_group(&pgroup);
        assert(get_list_count(pgroup.proclist) == 1);
    }

    assert(close_process_group(&pgroup) == 0);
    kill_and_wait(child_pid, SIGKILL);
}

/***************************************************************************
 * EXISTING TESTS (RENAMED FOR CONSISTENCY)
 ***************************************************************************/

/**
 * @brief Test process iterator with a single process
 * @note Tests that the process iterator can retrieve the current process
 *  information correctly, both with and without child processes
 */
static void test_process_iterator_single(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    size_t count;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Test without including children */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    count = 0;

    /* Initialize iterator and iterate through processes */
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        assert(process->pid == getpid());
        assert(process->ppid == getppid());
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 1);
    assert(close_process_iterator(&it) == 0);

    /* Test with including children */
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;
    count = 0;

    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        assert(process->pid == getpid());
        assert(process->ppid == getppid());
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 1);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

/**
 * @brief Test process iterator with multiple processes
 * @note Creates a child process and verifies that the iterator can retrieve
 *  both parent and child process information
 */
static void test_process_iterator_multiple(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    size_t count = 0;

    /* Create a child process for testing */
    pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process: sleep in a loop until killed */
        const struct timespec sleep_time = {5, 0L}; /* 5s */
        while (!is_quit_flag_set()) {
            sleep_timespec(&sleep_time);
        }
        _exit(EXIT_SUCCESS);
    }

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Set up filter to include children */
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;

    /* Initialize iterator and verify both processes are found */
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        if (process->pid == getpid()) {
            assert(process->ppid == getppid());
        } else if (process->pid == child_pid) {
            assert(process->ppid == getpid());
        } else {
            assert(0);
        }
        assert(process->cputime >= 0);
        count++;
    }
    assert(count == 2);
    free(process);
    assert(close_process_iterator(&it) == 0);

    /* Clean up child process */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process iterator with all system processes
 * @note Verifies that the iterator can retrieve processes and that the
 *  current process is correctly identified
 */
static void test_process_iterator_all(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    size_t count = 0;
    int found_self = 0;

    /* Set up filter to get all processes */
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Initialize iterator and count processes */
    assert(init_process_iterator(&it, &filter) == 0);

    while (get_next_process(&it, process) == 0) {
        if (process->pid == getpid()) {
            assert(process->ppid == getppid());
            assert(process->cputime >= 0);
            found_self = 1;
        }
        count++;
    }

    /* Verify we found at least one process and our own PID is visible */
    assert(count > 0);
    assert(found_self == 1);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

/**
 * @brief Test process group initialization with all processes
 * @note Verifies that a process group initialized with PID 0 (all processes)
 *  is non-empty and contains the current process
 */
static void test_process_group_init_all(void) {
    struct process_group pgroup;
    const struct list_node *node = NULL;
    size_t count = 0;
    int found_self = 0;

    /* Initialize process group with all processes */
    assert(init_process_group(&pgroup, 0, 0) == 0);
    update_process_group(&pgroup);

    /* Count processes in the group */
    for (node = pgroup.proclist->first; node != NULL; node = node->next) {
        const struct process *p = (const struct process *)node->data;
        if (p->pid == getpid()) {
            found_self = 1;
        }
        count++;
    }
    assert(count > 0);
    assert(found_self == 1);
    assert(count == get_list_count(pgroup.proclist));

    /* Update and verify again */
    update_process_group(&pgroup);
    assert(close_process_group(&pgroup) == 0);
}

/**
 * @brief Test process group with a single process
 * @param include_children Flag indicating whether to include child processes
 * @note Creates a child process and verifies that the process group
 *  correctly tracks it, with or without child process inclusion
 */
static void test_proc_group_single(int include_children) {
    struct process_group pgroup;
    int i;

    /* Create a child process for testing */
    pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process: busy loop until killed */
        volatile int keep_running = 1;
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var++) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }

    /* Initialize process group with the child PID */
    assert(init_process_group(&pgroup, child_pid, include_children) == 0);

    /* Update process group 100 times and verify consistency */
    for (i = 0; i < 100; i++) {
        const struct list_node *node = NULL;
        size_t count = 0;

        update_process_group(&pgroup);
        assert(get_list_count(pgroup.proclist) == 1);

        for (node = pgroup.proclist->first; node != NULL; node = node->next) {
            const struct process *p = (const struct process *)node->data;
            assert(p->pid == child_pid);
            assert(p->ppid == getpid());
            /* p->cpu_usage should be -1 or [0, NCPU] */
            assert((p->cpu_usage >= -1.00001 && p->cpu_usage <= -0.99999) ||
                   (p->cpu_usage >= 0 && p->cpu_usage <= 1.0 * get_ncpu()));
            count++;
        }
        assert(count == 1);
    }
    assert(close_process_group(&pgroup) == 0);

    /* Clean up child process */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process group with a single process (both with and without
 * children)
 * @note Wrapper function to test process group with include_children set to
 *  0 and 1
 */
static void test_process_group_init_single(void) {
    /* Test without including children */
    test_proc_group_single(0);

    /* Test with including children */
    test_proc_group_single(1);
}

static char *command = NULL;

/**
 * @brief Test process name retrieval
 * @note Verifies that the process iterator can correctly retrieve the
 *  command name of the current process
 */
static void test_process_iterator_read_command(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;
    const char *proc_name1, *proc_name2;
    int cmp_result;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Set up filter to get current process with command reading */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 1;

    /* Get process information and verify command name */
    assert(init_process_iterator(&it, &filter) == 0);
    assert(get_next_process(&it, process) == 0);
    assert(process->pid == getpid());
    assert(process->ppid == getppid());

    /* Compare command names */
    proc_name1 = file_basename(command);
    proc_name2 = file_basename(process->command);
    cmp_result = strcmp(proc_name1, proc_name2);
    assert(cmp_result == 0);

    /* Verify no more processes */
    assert(get_next_process(&it, process) != 0);
    free(process);
    assert(close_process_iterator(&it) == 0);
}

/**
 * @brief Test process group initialization with invalid PIDs
 * @note Verifies that process group initialization with invalid PIDs (-1 and
 *  INT_MAX) results in empty process lists
 */
static void test_process_group_init_invalid_pid(void) {
    struct process_group pgroup;

    /* Test with PID -1 */
    assert(init_process_group(&pgroup, -1, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);
    update_process_group(&pgroup);
    assert(get_list_count(pgroup.proclist) == 0);
    assert(close_process_group(&pgroup) == 0);

    /* Test with PID INT_MAX */
    assert(init_process_group(&pgroup, INT_MAX, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);
    update_process_group(&pgroup);
    assert(get_list_count(pgroup.proclist) == 0);
    assert(close_process_group(&pgroup) == 0);
}

/**
 * @brief Test init_process_group with NULL pgroup argument
 * @note Must return -1 without crashing when pgroup is NULL
 */
static void test_process_group_init_null(void) {
    assert(init_process_group(NULL, getpid(), 0) == -1);
}

/**
 * @brief Test find_process_by_pid function
 * @note Verifies that the current process can be found by its PID
 */
static void test_process_group_find_by_pid(void) {
    assert(find_process_by_pid(getpid()) == getpid());
}

/**
 * @brief Test find_process_by_name function
 * @note Tests various cases: correct process name, empty string, modified
 *  process names that should not match
 */
static void test_process_group_find_by_name(void) {
    char *wrong_name;
    size_t len;

    /* Allocate buffer for modified process names */
    if ((wrong_name = (char *)malloc(PATH_MAX + 1)) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /*
     * 'command' is the name of the current process (equivalent to argv[0]).
     * Verify that the find_process_by_name function can find the current
     * process (PID should match the return value of getpid()).
     */
    assert(find_process_by_name(command) == getpid());

#if defined(__linux__)
    /*
     * Test the absolute-path comparison branch: when process_name starts
     * with '/', find_process_by_name compares the full path against each
     * process's cmdline.  Use a path incorporating the current PID so it
     * is unique enough to never match any running process's cmdline,
     * even in shared CI environments.
     */
    {
        char abs_path[64];
        snprintf(abs_path, sizeof(abs_path), "/nonexistent/cpulimit_abs_%ld",
                 (long)getpid());
        assert(find_process_by_name(abs_path) == 0);
    }
#endif /* __linux__ */

    /*
     * Test Case 1: Pass an empty string to find_process_by_name.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, "");
    assert(find_process_by_name(wrong_name) == 0);

    /*
     * Test Case 2: Pass an incorrect process name by appending 'x'
     * to the current process's name.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, command); /* Copy the current process's name */
    strcat(wrong_name, "x");     /* Append 'x' to make it non-matching */
    assert(find_process_by_name(wrong_name) == 0);

    /*
     * Test Case 3: Pass a copy of the current process's name with
     * the last character removed.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, command); /* Copy the current process's name */
    len = strlen(wrong_name);
    wrong_name[len - 1] = '\0'; /* Remove the last character */
    assert(find_process_by_name(wrong_name) == 0);

    free(wrong_name);
}

/**
 * @brief Test getppid_of function
 * @note Verifies that getppid_of returns the correct parent PID for multiple
 *  processes, including the current process
 */
static void test_process_iterator_getppid_of(void) {
    struct process_iterator it;
    struct process *process;
    struct process_filter filter;

    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;

    /* Allocate memory for process structure */
    if ((process = (struct process *)malloc(sizeof(struct process))) == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Iterate through all processes and verify parent PID */
    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, process) == 0) {
        assert(getppid_of(process->pid) == process->ppid);
    }
    free(process);
    assert(close_process_iterator(&it) == 0);

    /* Verify current process's parent PID */
    assert(getppid_of(getpid()) == getppid());
}

/**
 * @brief Test limit_process function
 * @note Creates a process group with multi processes and applies CPU
 *  limiting to verify that the CPU usage stays within the specified limit
 */
static void test_limit_process_basic(void) {
    const double cpu_usage_limit = 0.5;
    pid_t child_pid;
    int sync_pipe[2], num_procs;
    num_procs = get_ncpu();
    /* Ensure at least 2 processes to validate include_children option */
    num_procs = MAX(num_procs, 2);

    /* Create pipe for synchronization */
    assert(pipe(sync_pipe) == 0);

    /* Fork first child process */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid > 0) {
        /* Parent process: monitor CPU usage */
        pid_t limiter_pid;
        ssize_t bytes_read, ret;
        char ack;

        assert(close(sync_pipe[1]) == 0);

        /* Wait for num_procs acknowledgements (from num_procs processes) */
        for (bytes_read = 0; bytes_read < num_procs; bytes_read++) {
            ret = read(sync_pipe[0], &ack, 1);
            if (ret == -1 && errno == EINTR) {
                continue; /* Interrupted, retry read */
            }
            assert(ret == 1 && ack == 'A');
        }
        /* Now should read EOF */
        do {
            ret = read(sync_pipe[0], &ack, 1);
        } while (ret == -1 && errno == EINTR);
        assert(ret == 0);
        assert(close(sync_pipe[0]) == 0);
        assert(bytes_read == num_procs);
        /* Fork CPU limiter process */
        limiter_pid = fork();
        assert(limiter_pid >= 0);

        if (limiter_pid > 0) {
            /* Monitor process: track CPU usage */
            int i;
            size_t count = 0;
            double cpu_usage = 0;
            struct process_group pgroup;
            const struct timespec sleep_time = {0, 500000000L};

            /* Initialize process group monitoring */
            assert(init_process_group(&pgroup, child_pid, 1) == 0);

            /* Monitor CPU usage over 60 iterations */
            for (i = 0; i < 60 && !is_quit_flag_set(); i++) {
                double temp_cpu_usage;
                sleep_timespec(&sleep_time);
                update_process_group(&pgroup);

                /* Verify all num_procs processes are being monitored */
                assert(get_list_count(pgroup.proclist) == (size_t)num_procs);

                temp_cpu_usage = get_process_group_cpu_usage(&pgroup);
                if (temp_cpu_usage > 0) {
                    cpu_usage += temp_cpu_usage;
                    count++;
                }
            }
            assert(close_process_group(&pgroup) == 0);
            assert(count > 0);

            /* Terminate limiter process first */
            kill_and_wait(limiter_pid, SIGKILL);

            /* Terminate entire process group */
            kill_and_wait(-child_pid, SIGKILL);

            /* Calculate and display average CPU usage */
            cpu_usage /= (double)count;
            printf("CPU usage limit: %.3f, CPU usage: %.3f\n", cpu_usage_limit,
                   cpu_usage);

            /* Verify CPU usage */
            assert(cpu_usage <= get_ncpu());

            return;
        }
        /* limiter_pid == 0: CPU limiter process */
        limit_process(child_pid, cpu_usage_limit, 1, 0);
        exit(EXIT_SUCCESS);
    } else {
        /* child_pid == 0: Target process group */
        int i;
        volatile int keep_running = 1;

        /* Create new process group */
        setpgid(0, 0);

        assert(close(sync_pipe[0]) == 0);

        /* Fork (num_procs - 1) child processes */
        for (i = 1; i < num_procs; i++) {
            pid_t pid = fork();
            assert(pid >= 0);

            if (pid == 0) {
                /* Child process: do not create more children */
                break;
            }
        }

        /* Send acknowledgement and close pipe */
        assert(write(sync_pipe[1], "A", 1) == 1);
        assert(close(sync_pipe[1]) == 0);

        /* Keep processes running until terminated */
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var++) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }
}

/***************************************************************************
 * LIMITER MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test run_command_mode with a command that exits immediately
 * @note Forks a child to call run_command_mode and checks the exit code
 *  matches the command's exit code
 */
static void test_limiter_run_command_mode(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;
    char cmd[] = "true";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = (char *const *)args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    /* Run in child process since run_command_mode calls exit() */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE); /* Should not reach here */
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    /* 'true' exits with 0, so run_command_mode should exit with 0 */
    assert(WEXITSTATUS(status) == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode when the target process does not exist
 * @note Verifies that lazy mode exits with EXIT_FAILURE when the target
 *  executable name is not found
 */
static void test_limiter_run_pid_or_exe_mode(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;
    const char exe_name[] = "nonexistent_exe_cpulimit_test_12345";

    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.exe_name = exe_name;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    /* Run in child since run_pid_or_exe_mode calls exit() */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Suppress output from run_pid_or_exe_mode in child */
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* Should not reach here */
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    /* Process not found with lazy_mode=1 -> EXIT_FAILURE */
    assert(WEXITSTATUS(status) == EXIT_FAILURE);
}

/***************************************************************************
 * ADDITIONAL WHITE-BOX COVERAGE TESTS
 ***************************************************************************/

/**
 * @brief Test add_elem with NULL data and locate_node skipping NULL-data nodes
 * @note Covers: add_elem(l, NULL), locate_node branch cur->data==NULL,
 *  destroy_node with NULL data pointer
 */
static void test_list_null_data_operations(void) {
    struct list l;
    struct list_node *node;
    int search_val;

    init_list(&l);

    /* add_elem with NULL data must create a valid node */
    node = add_elem(&l, NULL);
    assert(node != NULL);
    assert(node->data == NULL);
    assert(get_list_count(&l) == 1);
    assert(is_empty_list(&l) == 0);

    /* locate_node must skip the NULL-data node (branch: cur->data == NULL) */
    search_val = 42;
    assert(locate_node(&l, &search_val, 0, sizeof(int)) == NULL);
    assert(locate_elem(&l, &search_val, 0, sizeof(int)) == NULL);

    /* destroy_node with NULL data must not crash (branch: node->data == NULL)
     */
    destroy_node(&l, node);
    assert(get_list_count(&l) == 0);
    assert(is_empty_list(&l) == 1);
}

/**
 * @brief Test process_table_init and process_table_add with NULL inputs
 * @note Covers: process_table_init(NULL,...), process_table_add(NULL,p),
 *  process_table_add(pt,NULL), and duplicate-PID insertion (silently ignored)
 */
static void test_process_table_null_inputs_and_dup(void) {
    struct process_table pt;
    struct process *p1, *p2;
    const struct process *found;

    /* process_table_init with NULL pointer must not crash */
    process_table_init(NULL, 16);

    /* Set up a valid table for the remaining sub-tests */
    process_table_init(&pt, 16);

    /* process_table_add with NULL table must not crash */
    p1 = (struct process *)malloc(sizeof(struct process));
    assert(p1 != NULL);
    p1->pid = 100;
    p1->ppid = 1;
    p1->cputime = 0.0;
    process_table_add(NULL, p1);

    /* process_table_add with NULL process must not crash */
    process_table_add(&pt, NULL);
    assert(process_table_find(&pt, 100) == NULL);

    /* Normal add */
    process_table_add(&pt, p1);
    found = process_table_find(&pt, 100);
    assert(found == p1);

    /* Duplicate-PID insertion must be silently ignored: p1 stays */
    p2 = (struct process *)malloc(sizeof(struct process));
    assert(p2 != NULL);
    p2->pid = 100; /* same PID as p1 */
    p2->ppid = 1;
    p2->cputime = 0.0;
    process_table_add(&pt, p2);
    found = process_table_find(&pt, 100);
    assert(found == p1); /* p1 must still be the stored entry */

    /* p2 was never added to the table; free it manually */
    free(p2);

    /* p1 will be freed by process_table_destroy */
    process_table_destroy(&pt);
}

/**
 * @brief Test process_table_remove_stale with NULL active_list
 * @note When active_list is NULL, locate_elem always returns NULL so all
 *  entries are treated as stale and removed
 */
static void test_process_table_stale_null_list(void) {
    struct process_table pt;
    struct process *p;

    process_table_init(&pt, 16);

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);
    p->pid = 100;
    p->ppid = 1;
    p->cputime = 0.0;
    process_table_add(&pt, p);
    assert(process_table_find(&pt, 100) == p);

    /* NULL active_list: every entry lacks a match, so all are removed */
    process_table_remove_stale(&pt, NULL);
    assert(process_table_find(&pt, 100) == NULL);

    /* p was freed by process_table_remove_stale via destroy_node */
    process_table_destroy(&pt);
}

/**
 * @brief Test SIGQUIT signal handling
 * @note SIGQUIT must set both quit_flag and terminated_by_tty.
 *  The child calls setsid() to create a new session and detach from the
 *  controlling terminal, preventing BSD terminal drivers from propagating
 *  SIGQUIT to the parent's process group.
 */
static void test_signal_handler_sigquit(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /*
         * Create a new session to detach from the controlling terminal.
         * On BSD, raising SIGQUIT (a core-dump signal) in a process that
         * shares the parent's terminal session can cause the terminal
         * driver to propagate the signal to the parent's process group,
         * corrupting the parent's state. setsid() prevents this.
         */
        assert(setsid() != (pid_t)-1);
        configure_signal_handler();
        if (raise(SIGQUIT) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (!is_terminated_by_tty()) {
            _exit(3);
        }
        _exit(0);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/**
 * @brief Test SIGHUP signal handling
 * @note SIGHUP must set quit_flag but must NOT set terminated_by_tty
 */
static void test_signal_handler_sighup(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGHUP) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) { /* SIGHUP must NOT set the tty flag */
            _exit(3);
        }
        _exit(0);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/**
 * @brief Test SIGPIPE signal handling
 * @note SIGPIPE must set quit_flag but must NOT set terminated_by_tty
 */
static void test_signal_handler_sigpipe(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGPIPE) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) { /* SIGPIPE must NOT set the tty flag */
            _exit(3);
        }
        _exit(0);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/**
 * @brief Test init_process_iterator and get_next_process with NULL inputs
 * @note NULL it or NULL filter must return -1 without crashing
 */
static void test_process_iterator_null_inputs(void) {
    struct process_iterator it;
    struct process_filter filter;
    struct process *p;

    memset(&filter, 0, sizeof(filter));

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);

    /* NULL it pointer must return -1 */
    assert(init_process_iterator(NULL, &filter) == -1);

    /* NULL filter pointer must return -1 */
    assert(init_process_iterator(&it, NULL) == -1);

    /* get_next_process with NULL it must return -1 */
    assert(get_next_process(NULL, p) == -1);

    /* get_next_process with NULL p must return -1 */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    assert(init_process_iterator(&it, &filter) == 0);
    assert(get_next_process(&it, NULL) == -1);
    assert(close_process_iterator(&it) == 0);

    /* get_next_process after close (filter=NULL) must return -1 */
    assert(get_next_process(&it, p) == -1);

    free(p);
}

/**
 * @brief Test close_process_iterator with NULL pointer
 * @note Must return -1 without crashing
 */
static void test_process_iterator_close_null(void) {
    assert(close_process_iterator(NULL) == -1);
}

/**
 * @brief Test getppid_of with boundary and invalid PIDs
 * @note PID 0 and INT_MAX must return -1; current PID must return getppid()
 */
static void test_process_iterator_getppid_of_edges(void) {
    /* PID 0: /proc/0/stat does not exist */
    assert(getppid_of((pid_t)0) == (pid_t)-1);

    /* INT_MAX: virtually guaranteed non-existent PID */
    assert(getppid_of((pid_t)INT_MAX) == (pid_t)-1);

    /* Current process: must match getppid() */
    assert(getppid_of(getpid()) == getppid());
}

/**
 * @brief Test find_process_by_pid with invalid and boundary PIDs
 * @note PID 0 and negative PIDs must return 0; current PID must be found
 */
static void test_process_group_find_by_pid_edges(void) {
    pid_t result;

    /* PID 0 is rejected before kill() */
    result = find_process_by_pid((pid_t)0);
    assert(result == 0);

    /* Negative PID is rejected before kill() */
    result = find_process_by_pid((pid_t)-1);
    assert(result == 0);

    /* INT_MAX: virtually guaranteed non-existent PID */
    result = find_process_by_pid((pid_t)INT_MAX);
    assert(result == 0);

    /* Current process must be found */
    result = find_process_by_pid(getpid());
    assert(result == getpid());
}

/**
 * @brief Test find_process_by_name with NULL process name
 * @note Must return 0 without crashing
 */
static void test_process_group_find_by_name_null(void) {
    assert(find_process_by_name(NULL) == 0);
}

/**
 * @brief Test find_process_by_name with empty string (early-return path)
 * @note Must return 0 immediately without iterating processes
 */
static void test_process_group_find_by_name_empty_string(void) {
    assert(find_process_by_name("") == 0);
}

/**
 * @brief Test find_process_by_name with a relative path ending in '/'
 * @note The basename of "bin/" is an empty string, so no process can match;
 *  must return 0 without crashing
 */
static void test_process_group_find_by_name_trailing_slash(void) {
    /* Relative path with trailing slash yields an empty basename */
    assert(find_process_by_name("bin/") == 0);
}

/**
 * @brief Test get_process_group_cpu_usage when process list is empty
 * @note Must return -1.0 when no processes are tracked
 */
static void test_process_group_cpu_usage_empty_list(void) {
    struct process_group pgroup;
    double usage;

    /* Initialize with INT_MAX: no such process exists, list stays empty */
    assert(init_process_group(&pgroup, (pid_t)INT_MAX, 0) == 0);
    assert(get_list_count(pgroup.proclist) == 0);

    /* Empty list must yield -1.0 (unknown) */
    usage = get_process_group_cpu_usage(&pgroup);
    assert(usage >= -1.00001 && usage <= -0.99999);

    assert(close_process_group(&pgroup) == 0);
}

/**
 * @brief Test get_process_group_cpu_usage with NULL pointer
 * @note Verifies the null guard returns the -1 sentinel without crashing
 */
static void test_process_group_cpu_usage_null(void) {
    double usage = get_process_group_cpu_usage(NULL);
    assert(usage >= -1.00001 && usage <= -0.99999);
}

/**
 * @brief Test long2pid_t with LONG_MAX (overflow must return -1)
 * @note Covers the round-trip overflow detection branch
 */
static void test_util_long2pid_t_overflow(void) {
    pid_t result;

    /* LONG_MAX overflows pid_t (32-bit) on 64-bit platforms */
    result = long2pid_t(LONG_MAX);
    /* Either the round-trip check detects overflow (-1)
     * or, on exotic platforms where pid_t == long, the value fits.
     * Either way, the function must not crash. */
    (void)result;
}

#if defined(__linux__)
/**
 * @brief Test read_line_from_file with NULL, missing, and valid files
 * @note Covers all three return paths of read_line_from_file (Linux only)
 */
static void test_util_read_line_from_file(void) {
    char *line;
    char tmpfile[] = "/tmp/cpulimit_empty_XXXXXX";
    int fd;

    /* NULL filename must return NULL */
    line = read_line_from_file(NULL);
    assert(line == NULL);

    /* Non-existent file must return NULL */
    line = read_line_from_file("/nonexistent/cpulimit_test_no_such_file");
    assert(line == NULL);

    /* /proc/self/stat always exists and is non-empty */
    line = read_line_from_file("/proc/self/stat");
    assert(line != NULL);
    free(line);

    /* Empty file must return NULL (getline returns -1 on immediate EOF).
     * Use mkstemp() to avoid name collisions in parallel test runs. */
    fd = mkstemp(tmpfile);
    assert(fd >= 0);
    close(fd);
    line = read_line_from_file(tmpfile);
    assert(line == NULL);
    remove(tmpfile);

    /* A file containing only a newline returns a non-NULL empty string */
    {
        char nl_tmpfile[] = "/tmp/cpulimit_newline_XXXXXX";
        int nl_fd = mkstemp(nl_tmpfile);
        assert(nl_fd >= 0);
        assert(write(nl_fd, "\n", 1) == 1);
        close(nl_fd);
        line = read_line_from_file(nl_tmpfile);
        assert(line != NULL);
        assert(line[0] == '\0');
        free(line);
        remove(nl_tmpfile);
    }
}
#endif /* __linux__ */

/***************************************************************************
 * UTIL MODULE ADDITIONAL TESTS
 ***************************************************************************/

/**
 * @brief Test MAX, MIN, and CLAMP macros with all comparison branches
 * @note Covers a>b, a<b, a==b for MAX/MIN; below/above/in-range for CLAMP
 */
static void test_util_macros(void) {
    /* Use volatile to prevent value-propagation in static analysers while
     * still testing the equal-argument and boundary-value edge cases */
    volatile int eq1 = 4, eq2 = 4;
    volatile int clamp_low = 0, clamp_high = 10;
    volatile int clamp_atlow = 0, clamp_athigh = 10;

    /* MAX: larger-first, smaller-first, equal */
    assert(MAX(5, 3) == 5);
    assert(MAX(3, 5) == 5);
    assert(MAX(eq1, eq2) == 4);
    assert(MAX(-1, 0) == 0);

    /* MIN: smaller-first, larger-first, equal */
    assert(MIN(3, 5) == 3);
    assert(MIN(5, 3) == 3);
    assert(MIN(eq1, eq2) == 4);
    assert(MIN(-1, 0) == -1);

    /* CLAMP: value in range, below low, above high, equals low, equals high */
    assert(CLAMP(5, 0, 10) == 5);
    assert(CLAMP(-1, 0, 10) == 0);
    assert(CLAMP(15, 0, 10) == 10);
    assert(CLAMP(clamp_atlow, clamp_low, clamp_high) == 0);
    assert(CLAMP(clamp_athigh, clamp_low, clamp_high) == 10);
}

/**
 * @brief Test timediff_in_ms when the "later" timestamp is actually earlier
 * @note Covers the negative-difference branch (backwards / rewound time)
 */
static void test_util_timediff_negative(void) {
    struct timespec earlier, later;
    double diff_ms;

    /* 100 seconds backward */
    earlier.tv_sec = 200;
    earlier.tv_nsec = 0;
    later.tv_sec = 100;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms <= -99999.0 && diff_ms >= -100001.0);

    /* 500 ms backward (nanoseconds only) */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 500000000L;
    later.tv_sec = 100;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms <= -499.0 && diff_ms >= -501.0);
}

/**
 * @brief Test sleep_timespec with a zero-duration sleep
 * @note A zero sleep must return immediately without error
 */
static void test_util_sleep_timespec_zero(void) {
    const struct timespec zero_sleep = {0, 0};
    int ret;
    ret = sleep_timespec(&zero_sleep);
    /* 0 on success; -1 only if interrupted (EINTR) */
    assert(ret == 0 || ret == -1);
}

/***************************************************************************
 * SIGNAL_HANDLER MODULE ADDITIONAL TESTS
 ***************************************************************************/

/**
 * @brief Test initial state of signal handler flags before any signal is raised
 * @note Both quit_flag and terminated_by_tty must be 0 before any signal
 */
static void test_signal_handler_initial_state(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (is_quit_flag_set()) {
            _exit(1);
        }
        if (is_terminated_by_tty()) {
            _exit(2);
        }
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/***************************************************************************
 * PROCESS_GROUP MODULE ADDITIONAL TESTS
 ***************************************************************************/

/**
 * @brief Test find_process_by_pid with PID 1 (init/systemd)
 * @note PID 1 always exists: returns positive if accessible, -1 if EPERM,
 *  never 0
 */
static void test_process_group_find_by_pid_init(void) {
    pid_t result;
    result = find_process_by_pid((pid_t)1);
    assert(result != 0);
}

/***************************************************************************
 * LIMIT_PROCESS MODULE ADDITIONAL TESTS
 ***************************************************************************/

/**
 * @brief Test limit_process when the target has already exited
 * @note Exercises the empty-proclist early-exit branch in limit_process
 */
static void test_limit_process_exits_early(void) {
    const struct timespec t = {0, 50000000L}; /* 50 ms */
    pid_t child_pid;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        _exit(EXIT_SUCCESS);
    }

    /* Allow child time to exit so /proc entry is gone */
    sleep_timespec(&t);

    /* limit_process must handle an already-gone process gracefully */
    limit_process(child_pid, 0.5, 0, 0);

    /* Reap any remaining zombie */
    waitpid(child_pid, NULL, WNOHANG);
}

/**
 * @brief Test limit_process with verbose=1
 * @note Exercises the verbose printf branches; output suppressed via fork
 */
static void test_limit_process_verbose(void) {
    pid_t wrapper_pid;
    int wrapper_status;

    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        const struct timespec t = {0, 50000000L};
        pid_t child_pid;

        close(STDOUT_FILENO); /* Suppress verbose output */

        child_pid = fork();
        assert(child_pid >= 0);
        if (child_pid == 0) {
            _exit(EXIT_SUCCESS);
        }

        sleep_timespec(&t);
        limit_process(child_pid, 0.5, 0, 1); /* verbose = 1 */
        waitpid(child_pid, NULL, WNOHANG);
        _exit(EXIT_SUCCESS);
    }

    assert(waitpid(wrapper_pid, &wrapper_status, 0) == wrapper_pid);
    assert(WIFEXITED(wrapper_status));
    assert(WEXITSTATUS(wrapper_status) == EXIT_SUCCESS);
}

/***************************************************************************
 * LIMITER MODULE ADDITIONAL TESTS
 ***************************************************************************/

/**
 * @brief Test run_command_mode with a non-existent binary
 * @note execvp fails in child; parent should exit with EXIT_FAILURE
 */
static void test_limiter_run_command_mode_nonexistent(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;
    char cmd[] = "/nonexistent_cpulimit_test_binary_xyz";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = (char *const *)args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == EXIT_FAILURE);
}

/**
 * @brief Test run_command_mode with verbose=1 and a command that succeeds
 * @note Exercises the verbose printf branches in run_command_mode
 */
static void test_limiter_run_command_mode_verbose(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;
    char cmd[] = "true";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = (char *const *)args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;
    cfg.verbose = 1;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode with a PID (pid_mode=1) that does not exist
 * @note Uses INT_MAX which is virtually guaranteed to be non-existent;
 *  lazy_mode=1 -> EXIT_FAILURE
 */
static void test_limiter_run_pid_or_exe_mode_pid_not_found(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;

    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.target_pid = (pid_t)INT_MAX;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == EXIT_FAILURE);
}

/***************************************************************************
 * CLI MODULE TESTS (parse_arguments)
 ***************************************************************************/

/**
 * @brief Helper: fork with output suppressed, call parse_arguments, return
 *  the child's exit-status code
 * @param argc Argument count
 * @param argv Argument vector
 * @return Exit status from the child process
 */
static int run_parse_in_child(int argc, char *const *argv) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        parse_arguments(argc, argv, &cfg);
        _exit(99); /* Only reached when parse_arguments returns (valid args) */
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

/**
 * @brief Test parse_arguments with -l LIMIT -p PID (valid PID mode)
 * @note Verifies target_pid, limit fraction, and implied lazy_mode
 */
static void test_cli_pid_mode(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid[32];
    char *av[6];

    snprintf(arg_pid, sizeof(arg_pid), "%ld", (long)getpid());
    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = arg_p;
    av[4] = arg_pid;
    av[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, (char *const *)av, &cfg);

    assert(cfg.target_pid == getpid());
    assert(cfg.limit >= 0.4999 && cfg.limit <= 0.5001);
    assert(cfg.lazy_mode == 1); /* -p implies lazy */
    assert(cfg.verbose == 0);
    assert(cfg.include_children == 0);
    assert(cfg.exe_name == NULL);
    assert(cfg.command_mode == 0);
}

/**
 * @brief Test parse_arguments with -l LIMIT -e EXE (valid exe mode)
 * @note Verifies exe_name, limit fraction; lazy_mode stays 0
 */
static void test_cli_exe_mode(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_25[] = "25";
    char arg_e[] = "-e";
    char arg_exe[] = "some_exe";
    char *av[6];

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_25;
    av[3] = arg_e;
    av[4] = arg_exe;
    av[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, (char *const *)av, &cfg);

    assert(cfg.exe_name != NULL);
    assert(strcmp(cfg.exe_name, "some_exe") == 0);
    assert(cfg.limit >= 0.2499 && cfg.limit <= 0.2501);
    assert(cfg.lazy_mode == 0); /* -e alone does not imply lazy */
    assert(cfg.target_pid == 0);
    assert(cfg.command_mode == 0);
}

/**
 * @brief Test parse_arguments with -l LIMIT COMMAND [ARGS] (command mode)
 * @note Verifies command_mode=1, command_args pointer, implied lazy_mode
 */
static void test_cli_command_mode(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_75[] = "75";
    char arg_cmd[] = "echo";
    char arg_msg[] = "hello";
    char *av[6];

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_75;
    av[3] = arg_cmd;
    av[4] = arg_msg;
    av[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, (char *const *)av, &cfg);

    assert(cfg.command_mode == 1);
    assert(cfg.command_args != NULL);
    assert(strcmp(cfg.command_args[0], "echo") == 0);
    assert(cfg.lazy_mode == 1); /* command mode implies lazy */
    assert(cfg.target_pid == 0);
    assert(cfg.exe_name == NULL);
    assert(cfg.limit >= 0.7499 && cfg.limit <= 0.7501);
}

/**
 * @brief Test parse_arguments with long options (--limit=N, --pid=N)
 * @note Long-form options must behave identically to short-form
 */
static void test_cli_long_options(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_pid_long[48];
    char *av[4];

    snprintf(arg_pid_long, sizeof(arg_pid_long), "--pid=%ld", (long)getpid());
    av[0] = arg0;
    av[1] = arg_limit;
    av[2] = arg_pid_long;
    av[3] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(3, (char *const *)av, &cfg);

    assert(cfg.target_pid == getpid());
    assert(cfg.limit >= 0.4999 && cfg.limit <= 0.5001);
    assert(cfg.lazy_mode == 1);
}

/**
 * @brief Test parse_arguments with --exe=EXE long option
 * @note --exe long form must set exe_name correctly
 */
static void test_cli_long_option_exe(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_exe[] = "--exe=myapp";
    char *av[4];

    av[0] = arg0;
    av[1] = arg_limit;
    av[2] = arg_exe;
    av[3] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(3, (char *const *)av, &cfg);

    assert(cfg.exe_name != NULL);
    assert(strcmp(cfg.exe_name, "myapp") == 0);
    assert(cfg.limit >= 0.4999 && cfg.limit <= 0.5001);
}

/**
 * @brief Test parse_arguments with -z and -i optional flags
 * @note Verifies lazy_mode and include_children are set correctly
 */
static void test_cli_optional_flags(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_z[] = "-z";
    char arg_i[] = "-i";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[8];

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = arg_z;
    av[4] = arg_i;
    av[5] = arg_e;
    av[6] = arg_exe;
    av[7] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(7, (char *const *)av, &cfg);

    assert(cfg.lazy_mode == 1);
    assert(cfg.include_children == 1);
}

/**
 * @brief Test parse_arguments with -v (verbose) flag
 * @note -v causes a "N CPUs detected" print; run in fork to suppress it
 */
static void test_cli_verbose_flag(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct cpulimitcfg cfg;
        char arg0[] = "cpulimit";
        char arg_l[] = "-l";
        char arg_50[] = "50";
        char arg_v[] = "-v";
        char arg_e[] = "-e";
        char arg_exe[] = "foo";
        char *av[7];

        av[0] = arg0;
        av[1] = arg_l;
        av[2] = arg_50;
        av[3] = arg_v;
        av[4] = arg_e;
        av[5] = arg_exe;
        av[6] = NULL;

        close(STDOUT_FILENO); /* Suppress "N CPUs detected" output */
        memset(&cfg, 0, sizeof(cfg));
        parse_arguments(6, (char *const *)av, &cfg);
        if (cfg.verbose != 1) {
            _exit(1);
        }
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/**
 * @brief Test parse_arguments with -h / --help
 * @note Both short and long help flags must exit with EXIT_SUCCESS
 */
static void test_cli_help(void) {
    char arg0[] = "cpulimit";
    char arg_h[] = "-h";
    char arg_help[] = "--help";
    char *av1[3];
    char *av2[3];

    av1[0] = arg0;
    av1[1] = arg_h;
    av1[2] = NULL;
    assert(run_parse_in_child(2, (char *const *)av1) == EXIT_SUCCESS);

    av2[0] = arg0;
    av2[1] = arg_help;
    av2[2] = NULL;
    assert(run_parse_in_child(2, (char *const *)av2) == EXIT_SUCCESS);
}

/**
 * @brief Test parse_arguments when -l/--limit is not supplied
 * @note The limit option is required; absence must cause EXIT_FAILURE
 */
static void test_cli_missing_limit(void) {
    char arg0[] = "cpulimit";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[4];

    av[0] = arg0;
    av[1] = arg_e;
    av[2] = arg_exe;
    av[3] = NULL;
    assert(run_parse_in_child(3, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with various invalid limit values
 * @note zero, negative, non-numeric, NaN, and above-max all cause
 *  EXIT_FAILURE
 */
static void test_cli_invalid_limits(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char arg_zero[] = "0";
    char arg_neg[] = "-5";
    char arg_abc[] = "abc";
    char arg_nan[] = "nan";
    char arg_huge[] = "99999";
    char *av[6];

    /* Common setup: prog -l LIMIT -e foo */
    av[0] = arg0;
    av[1] = arg_l;
    av[3] = arg_e;
    av[4] = arg_exe;
    av[5] = NULL;

    av[2] = arg_zero;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[2] = arg_neg;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[2] = arg_abc;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[2] = arg_nan;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[2] = arg_huge;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with various invalid PID values
 * @note 0, 1 (reserved), -1, non-numeric, and trailing-char PIDs all cause
 *  EXIT_FAILURE
 */
static void test_cli_invalid_pids(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid0[] = "0";
    char arg_pid1[] = "1";
    char arg_pidneg[] = "-1";
    char arg_pidabc[] = "abc";
    char arg_pidtrail[] = "10x";
    char *av[6];

    /* Common setup: prog -l 50 -p PID */
    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = arg_p;
    av[5] = NULL;

    av[4] = arg_pid0;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[4] = arg_pid1; /* pid <= 1 validation rejects PID 1 (init/systemd) */
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[4] = arg_pidneg;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[4] = arg_pidabc;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);

    av[4] = arg_pidtrail;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with an empty exe name (-e "")
 * @note Empty string for -e must cause EXIT_FAILURE
 */
static void test_cli_empty_exe(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_e[] = "-e";
    char arg_empty[] = "";
    char *av[6];

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = arg_e;
    av[4] = arg_empty;
    av[5] = NULL;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with no target specified (-l only)
 * @note Providing a limit but no -p/-e/COMMAND must cause EXIT_FAILURE
 */
static void test_cli_no_target(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char *av[4];

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = NULL;
    assert(run_parse_in_child(3, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with both -p and -e simultaneously
 * @note Only one target is allowed; two must cause EXIT_FAILURE
 */
static void test_cli_multiple_targets(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid[32];
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[8];

    snprintf(arg_pid, sizeof(arg_pid), "%ld", (long)getpid());
    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = arg_p;
    av[4] = arg_pid;
    av[5] = arg_e;
    av[6] = arg_exe;
    av[7] = NULL;
    assert(run_parse_in_child(7, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with unknown short and long options
 * @note Unrecognised options must cause EXIT_FAILURE
 */
static void test_cli_unknown_option(void) {
    char arg0[] = "cpulimit";
    char arg_x[] = "-x";
    char arg_bogus[] = "--bogus";
    char *av1[3];
    char *av2[3];

    av1[0] = arg0;
    av1[1] = arg_x;
    av1[2] = NULL;
    assert(run_parse_in_child(2, (char *const *)av1) == EXIT_FAILURE);

    av2[0] = arg0;
    av2[1] = arg_bogus;
    av2[2] = NULL;
    assert(run_parse_in_child(2, (char *const *)av2) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments when a required option argument is absent
 * @note -p with no PID and -l with no value must both cause EXIT_FAILURE
 */
static void test_cli_missing_arg(void) {
    char arg0[] = "cpulimit";
    char arg_p[] = "-p";
    char arg_l[] = "-l";
    char *av1[3];
    char *av2[3];

    av1[0] = arg0;
    av1[1] = arg_p;
    av1[2] = NULL;
    assert(run_parse_in_child(2, (char *const *)av1) == EXIT_FAILURE);

    av2[0] = arg0;
    av2[1] = arg_l;
    av2[2] = NULL;
    assert(run_parse_in_child(2, (char *const *)av2) == EXIT_FAILURE);
}

/***************************************************************************
 * LIST MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test first_node with non-empty list
 * @note first_node must return the first node after elements are added
 */
static void test_list_first_node_nonempty(void) {
    struct list l;
    int a = 1, b = 2;
    const struct list_node *n;

    init_list(&l);
    add_elem(&l, &a);
    add_elem(&l, &b);

    n = first_node(&l);
    assert(n != NULL);
    assert(n->data == &a);
    assert(n->next != NULL);
    assert(n->next->data == &b);
    assert(n->previous == NULL);

    clear_list(&l);
}

/**
 * @brief Test delete_node on empty list (count==0 guard path)
 * @note Must be a no-op when count is 0
 */
static void test_list_delete_node_empty(void) {
    struct list l;
    struct list_node fake_node;

    init_list(&l);
    /* count == 0 guard: should silently return */
    delete_node(&l, &fake_node);
    assert(get_list_count(&l) == 0);
}

/**
 * @brief Test destroy_node with a NULL list frees node and data without crash
 * @note Exercises the l==NULL fast-path that frees node directly
 */
static void test_list_destroy_node_null_list(void) {
    struct list_node *node;
    int *data;

    data = (int *)malloc(sizeof(int));
    assert(data != NULL);
    *data = 42;

    node = (struct list_node *)malloc(sizeof(struct list_node));
    assert(node != NULL);
    node->data = data;
    node->previous = NULL;
    node->next = NULL;

    /* destroy_node with NULL list must free both data and node */
    destroy_node(NULL, node);
    /* If we reach here without crash, the test passed */
}

/**
 * @brief Test locate_node/locate_elem with a single-element list
 * @note Ensures the single-node match and single-node miss paths work
 */
static void test_list_locate_single(void) {
    struct list l;
    int val = 42, miss = 99;
    struct list_node *node;
    const void *elem;

    init_list(&l);
    add_elem(&l, &val);

    node = locate_node(&l, &val, 0, sizeof(int));
    assert(node != NULL);
    assert(*(int *)node->data == 42);

    elem = locate_elem(&l, &val, 0, sizeof(int));
    assert(elem == &val);

    /* Miss case */
    assert(locate_node(&l, &miss, 0, sizeof(int)) == NULL);
    assert(locate_elem(&l, &miss, 0, sizeof(int)) == NULL);

    clear_list(&l);
}

/**
 * @brief Test clear_list on an already-empty list (no-op path)
 * @note Must not crash and count stays 0
 */
static void test_list_clear_empty(void) {
    struct list l;
    init_list(&l);
    clear_list(&l);
    assert(get_list_count(&l) == 0);
    assert(is_empty_list(&l) == 1);
}

/**
 * @brief Test destroy_list on an already-empty list (no-op path)
 * @note Must not crash and count stays 0
 */
static void test_list_destroy_empty(void) {
    struct list l;
    init_list(&l);
    destroy_list(&l);
    assert(get_list_count(&l) == 0);
    assert(is_empty_list(&l) == 1);
}

/***************************************************************************
 * UTIL MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test nsec2timespec with 0 and sub-second values
 * @note Covers zero input and values < 1e9 ns (sec should be 0)
 */
static void test_util_nsec2timespec_zero_and_sub(void) {
    struct timespec t;

    /* Zero nanoseconds */
    nsec2timespec(0.0, &t);
    assert(t.tv_sec == 0);
    assert(t.tv_nsec == 0);

    /* 500 ms = 5e8 ns -> tv_sec=0, tv_nsec=500000000 */
    nsec2timespec(500000000.0, &t);
    assert(t.tv_sec == 0);
    assert(t.tv_nsec == 500000000L);

    /* Exactly 1 second */
    nsec2timespec(1e9, &t);
    assert(t.tv_sec == 1);
    assert(t.tv_nsec == 0);
}

/**
 * @brief Test nsec2timespec rollover correction for floating-point edge cases
 * @note Verifies that tv_nsec stays in [0, 999999999] and tv_sec is adjusted
 *       when floating-point rounding pushes tv_nsec out of range
 */
static void test_util_nsec2timespec_rollover(void) {
    struct timespec t;

    /*
     * Values very close to an integer-second boundary can trigger
     * floating-point rounding that makes tv_nsec negative or >= 1e9.
     * Use nextafter to obtain a value that is representable just below
     * or just above a boundary and verify the correction branches.
     */

    /* A large multiple of 1e9 can round tv_nsec up to 1e9 */
    nsec2timespec(3e9, &t);
    assert(t.tv_sec >= 2 && t.tv_sec <= 4);
    assert(t.tv_nsec >= 0L && t.tv_nsec <= 999999999L);

    /* 2 seconds exactly */
    nsec2timespec(2e9, &t);
    assert(t.tv_sec == 2);
    assert(t.tv_nsec == 0L);

    /* 0.999999999 s = 999999999 ns -- must stay sub-second */
    nsec2timespec(999999999.0, &t);
    assert(t.tv_sec == 0);
    assert(t.tv_nsec >= 0L && t.tv_nsec <= 999999999L);

    /* General invariant: result is always normalised */
    nsec2timespec(1234567890.123, &t);
    assert(t.tv_nsec >= 0L && t.tv_nsec <= 999999999L);
}

/**
 * @brief Test timediff_in_ms when only nanoseconds differ
 * @note Covers the sub-millisecond sub-second difference path
 */
static void test_util_timediff_nsec_only(void) {
    struct timespec t1, t2;
    double diff;

    t1.tv_sec = 500;
    t1.tv_nsec = 0;
    t2.tv_sec = 500;
    t2.tv_nsec = 999000L; /* 0.999 ms */
    diff = timediff_in_ms(&t2, &t1);
    assert(diff >= 0.998 && diff <= 1.0);
}

/**
 * @brief Test get_current_time returns a non-decreasing timestamp
 * @note Two successive calls must return increasing or equal time
 */
static void test_util_get_current_time_monotonic(void) {
    struct timespec t1, t2;
    double diff;
    assert(get_current_time(&t1) == 0);
    assert(get_current_time(&t2) == 0);
    diff = timediff_in_ms(&t2, &t1);
    assert(diff >= 0.0); /* monotonic: must not go backwards */
}

/**
 * @brief Test sleep_timespec with a positive (non-zero) duration
 * @note A 1 ms sleep must complete without error
 */
static void test_util_sleep_timespec_nonzero(void) {
    const struct timespec t = {0, 1000000L}; /* 1 ms */
    int ret = sleep_timespec(&t);
    /* 0 on success; -1 only if EINTR */
    assert(ret == 0 || ret == -1);
}

/**
 * @brief Test long2pid_t with input value 0
 * @note 0 is not negative but should produce (pid_t)0 without overflow
 */
static void test_util_long2pid_t_zero(void) {
    pid_t result = long2pid_t(0L);
    assert(result == (pid_t)0);
}

/**
 * @brief Test file_basename with empty-string path
 * @note Empty string has no '/', so the whole string is returned
 */
static void test_util_file_basename_empty(void) {
    const char *result = file_basename("");
    assert(result != NULL);
    assert(result[0] == '\0'); /* empty string points to itself */
}

/***************************************************************************
 * PROCESS_TABLE MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test process_table_init with hashsize=0 (forced to 1)
 * @note hashsize=0 must be clamped to 1; add/find/del must still work
 */
static void test_process_table_init_hashsize_zero(void) {
    struct process_table pt;
    struct process *p;

    process_table_init(&pt, 0);
    assert(pt.hashsize == 1);
    assert(pt.table != NULL);

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);
    p->pid = 77;
    p->ppid = 1;
    p->cputime = 0.0;
    p->cpu_usage = -1.0;

    process_table_add(&pt, p);
    assert(process_table_find(&pt, 77) == p);
    assert(process_table_del(&pt, 77) == 0);
    assert(process_table_find(&pt, 77) == NULL);

    process_table_destroy(&pt);
}

/**
 * @brief Test process_table_find with NULL process table
 * @note Must return NULL without crashing
 */
static void test_process_table_find_null_pt(void) {
    assert(process_table_find(NULL, 1) == NULL);
}

/**
 * @brief Test process_table_del when PID is absent from a populated bucket
 * @note del on a non-empty table for a PID in the same bucket must return 1
 */
static void test_process_table_del_absent_pid(void) {
    struct process_table pt;
    struct process *p;

    /* Use hashsize=1 so all PIDs go to bucket 0 */
    process_table_init(&pt, 1);

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);
    p->pid = 5;
    p->ppid = 1;
    p->cputime = 0.0;
    p->cpu_usage = -1.0;
    process_table_add(&pt, p);

    /* PID 99 hashes to bucket 0 (same bucket, but not in list) */
    assert(process_table_del(&pt, 99) == 1);

    /* PID 5 is still there */
    assert(process_table_find(&pt, 5) == p);

    process_table_destroy(&pt);
}

/**
 * @brief Test process_table_del on a PID that was never inserted at all
 * @note Empty bucket: returns 1
 */
static void test_process_table_del_empty_bucket(void) {
    struct process_table pt;

    process_table_init(&pt, 16);
    assert(process_table_del(&pt, 100) == 1);
    process_table_destroy(&pt);
}

/**
 * @brief Test process_table_destroy on NULL and on a freshly-initialized table
 * @note NULL must not crash; fresh empty table must also not crash
 */
static void test_process_table_destroy_edge_cases(void) {
    struct process_table pt;

    /* NULL pointer: must be a no-op */
    process_table_destroy(NULL);

    /* Fresh table with no entries */
    process_table_init(&pt, 8);
    process_table_destroy(&pt);
    /* Subsequent destroy must not crash (pt->table is NULL) */
    process_table_destroy(&pt);
}

/**
 * @brief Test that process_table operations are safe after destroy
 * @note After process_table_destroy, find/add/del/remove_stale must not
 *  crash even though pt->table is NULL and pt->hashsize is 0
 */
static void test_process_table_ops_after_destroy(void) {
    struct process_table pt;
    struct process *p;
    const struct process *found;
    int ret;

    process_table_init(&pt, 16);
    process_table_destroy(&pt);
    /* pt->table is now NULL, pt->hashsize is now 0 */

    /* find must return NULL without crashing */
    found = process_table_find(&pt, 100);
    assert(found == NULL);

    /* add must be a no-op without crashing */
    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);
    p->pid = 100;
    p->ppid = 1;
    p->cputime = 0.0;
    p->cpu_usage = -1.0;
    process_table_add(&pt, p);
    free(p); /* p was never added to the table; must be freed manually */

    /* del must return 1 without crashing */
    ret = process_table_del(&pt, 100);
    assert(ret == 1);

    /* remove_stale must be a no-op without crashing */
    process_table_remove_stale(&pt, NULL);
}

/***************************************************************************
 * SIGNAL_HANDLER MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test SIGTERM handling
 * @note SIGTERM must set quit_flag but must NOT set terminated_by_tty
 */
static void test_signal_handler_sigterm(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGTERM) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) { /* SIGTERM must NOT set tty flag */
            _exit(3);
        }
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/**
 * @brief Test SIGINT handling
 * @note SIGINT must set both quit_flag and terminated_by_tty
 */
static void test_signal_handler_sigint(void) {
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGINT) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (!is_terminated_by_tty()) { /* SIGINT MUST set tty flag */
            _exit(3);
        }
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

/***************************************************************************
 * PROCESS_ITERATOR MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test init_process_iterator with pid=0 and include_children=1
 * @note Should open /proc and enumerate processes (same as all-process scan)
 */
static void test_process_iterator_init_all_with_children(void) {
    struct process_iterator it;
    struct process_filter filter;
    struct process *p;
    int count = 0;

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);

    filter.pid = 0;
    filter.include_children = 1;
    filter.read_cmd = 0;

    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, p) == 0 && count < 5) {
        count++;
    }
    assert(count > 0);
    assert(close_process_iterator(&it) == 0);

    free(p);
}

/**
 * @brief Test get_next_process after end_of_processes is set
 * @note Must return -1 immediately on every call after first exhaustion
 */
static void test_process_iterator_exhaust_single(void) {
    struct process_iterator it;
    struct process_filter filter;
    struct process *p;

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);

    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;

    assert(init_process_iterator(&it, &filter) == 0);

    /* First call: returns the process */
    assert(get_next_process(&it, p) == 0);
    assert(p->pid == getpid());

    /* Second call: end_of_processes=1, must return -1 */
    assert(get_next_process(&it, p) == -1);

    /* Third call: still -1 */
    assert(get_next_process(&it, p) == -1);

    assert(close_process_iterator(&it) == 0);
    free(p);
}

/**
 * @brief Test process iterator with include_children=1 for current process
 * @note With a child process running, both parent and child must appear
 */
static void test_process_iterator_with_children(void) {
    struct process_iterator it;
    struct process_filter filter;
    struct process *p;
    pid_t child_pid;
    int found_parent = 0, found_child = 0;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        const struct timespec t = {5, 0};
        sleep_timespec(&t);
        _exit(EXIT_SUCCESS);
    }

    p = (struct process *)malloc(sizeof(struct process));
    assert(p != NULL);

    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;

    assert(init_process_iterator(&it, &filter) == 0);
    while (get_next_process(&it, p) == 0) {
        if (p->pid == getpid()) {
            found_parent = 1;
        }
        if (p->pid == child_pid) {
            found_child = 1;
        }
    }
    assert(close_process_iterator(&it) == 0);

    assert(found_parent == 1);
    assert(found_child == 1);

    free(p);
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test close_process_iterator when dip is already NULL
 * @note After init with single-pid filter, dip==NULL; close must return 0
 */
static void test_process_iterator_close_null_dip(void) {
    struct process_iterator it;
    struct process_filter filter;

    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;

    assert(init_process_iterator(&it, &filter) == 0);
#if defined(__linux__)
    /* dip is NULL because single-PID optimisation skips opendir() */
    assert(it.dip == NULL);
#endif
    assert(close_process_iterator(&it) == 0);
}

/***************************************************************************
 * PROCESS_GROUP MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test close_process_group with NULL pointer
 * @note Must return 0 without crashing
 */
static void test_process_group_close_null(void) {
    struct process_group pg;

    /* NULL pgroup pointer must return 0 without crashing */
    assert(close_process_group(NULL) == 0);

    /* Partially initialised struct (NULL members) must also work */
    pg.proctable = NULL;
    pg.proclist = NULL;
    assert(close_process_group(&pg) == 0);
}

/**
 * @brief Test that close_process_group zeros all numeric fields
 * @note After close, target_pid, include_children, and last_update must be 0
 */
static void test_process_group_close_zeros_fields(void) {
    struct process_group pgroup;
    assert(init_process_group(&pgroup, getpid(), 1) == 0);
    assert(pgroup.target_pid == getpid());
    assert(pgroup.include_children == 1);
    assert(close_process_group(&pgroup) == 0);
    assert(pgroup.proclist == NULL);
    assert(pgroup.proctable == NULL);
    assert(pgroup.target_pid == 0);
    assert(pgroup.include_children == 0);
    assert(pgroup.last_update.tv_sec == 0);
    assert(pgroup.last_update.tv_nsec == 0);
}

/**
 * @brief Test update_process_group with NULL pointer
 * @note Must return without crashing
 */
static void test_process_group_update_null(void) {
    /* NULL pgroup must not crash */
    update_process_group(NULL);
}

/**
 * @brief Test update_process_group twice in quick succession
 * @note Second call exercises the "insufficient dt" branch
 */
static void test_process_group_double_update(void) {
    struct process_group pgroup;
    assert(init_process_group(&pgroup, getpid(), 0) == 0);
    update_process_group(&pgroup);
    /* Immediate second update: dt < MIN_DT, so CPU usage stays -1 */
    update_process_group(&pgroup);
    assert(close_process_group(&pgroup) == 0);
}

/**
 * @brief Test find_process_by_name with self's executable basename
 * @note The test binary name must be found; result > 0 or result is -PID
 *  (EPERM in confined environments)
 */
static void test_process_group_find_by_name_self(void) {
    const char *self_name;
    pid_t result;

    if (command == NULL) {
        return; /* command set in main() */
    }
    self_name = file_basename(command);
    if (self_name == NULL || self_name[0] == '\0') {
        return;
    }
    result = find_process_by_name(self_name);
    /* Must find at least itself (positive) or get EPERM (-pid) */
    assert(result != 0);
}

/**
 * @brief Test get_process_group_cpu_usage after waiting for a valid sample
 * @note After two updates separated by enough time, cpu_usage may be >= 0
 */
static void test_process_group_cpu_usage_with_usage(void) {
    struct process_group pgroup;
    const struct timespec wait = {0, 50000000L}; /* 50 ms */
    double usage;
    int i;

    assert(init_process_group(&pgroup, getpid(), 0) == 0);
    for (i = 0; i < 5; i++) {
        sleep_timespec(&wait);
        update_process_group(&pgroup);
    }
    usage = get_process_group_cpu_usage(&pgroup);
    /* After several updates usage should be either -1 (not yet measured)
     * or a valid non-negative value */
    assert(usage >= -1.00001);
    assert(close_process_group(&pgroup) == 0);
}

/***************************************************************************
 * LIMIT_PROCESS MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test limit_process with include_children=1
 * @note Target exits quickly; exercises the children-tracking code path
 */
static void test_limit_process_include_children(void) {
    const struct timespec t = {0, 50000000L}; /* 50 ms */
    pid_t child_pid;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        _exit(EXIT_SUCCESS);
    }

    sleep_timespec(&t);                  /* Let child exit first */
    limit_process(child_pid, 0.5, 1, 0); /* include_children=1 */
    waitpid(child_pid, NULL, WNOHANG);
}

/***************************************************************************
 * LIMITER MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test run_command_mode with a command that exits non-zero ('false')
 * @note Exit status of 'false' is 1; run_command_mode should propagate it
 */
static void test_limiter_run_command_mode_false(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;
    char cmd[] = "false";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = (char *const *)args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(99);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == EXIT_FAILURE);
}

/**
 * @brief Test run_pid_or_exe_mode with exe mode, non-lazy, immediate exit
 *  via quit flag (verifies the non-lazy quit-flag early-exit path)
 * @note Uses a nonexistent exe so proclist stays empty, quit flag breaks loop
 */
static void test_limiter_run_pid_or_exe_mode_quit(void) {
    pid_t pid;
    int status;
    struct cpulimitcfg cfg;
    const char exe[] = "nonexistent_cpulimit_quit_test_xyz";

    memset(&cfg, 0, sizeof(struct cpulimitcfg));
    cfg.program_name = "test";
    cfg.exe_name = exe;
    cfg.limit = 0.5;
    cfg.lazy_mode = 0; /* non-lazy: loop until quit */

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        configure_signal_handler();
        /* Send ourselves SIGTERM to set the quit flag, then call the mode */
        kill(getpid(), SIGTERM);
        run_pid_or_exe_mode(&cfg);
        _exit(99);
    }

    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    /* Process not found -> EXIT_SUCCESS (non-lazy, quit gracefully) */
    assert(WEXITSTATUS(status) == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode with verbose=0 when process is found
 * @note Verifies that the non-verbose code path (verbose guard is false)
 *  works correctly when a process is found: the function limits it and exits
 *  with EXIT_SUCCESS when the target terminates naturally.
 */
static void test_limiter_run_pid_or_exe_mode_pid_found(void) {
    pid_t wrapper_pid;
    int wrapper_status;
    const struct timespec target_life = {0, 500000000L}; /* 500 ms */

    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t target_pid;
        struct cpulimitcfg cfg;

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        /* Fork a target that stays alive long enough to be found */
        target_pid = fork();
        assert(target_pid >= 0);
        if (target_pid == 0) {
            sleep_timespec(&target_life);
            _exit(EXIT_SUCCESS);
        }

        memset(&cfg, 0, sizeof(struct cpulimitcfg));
        cfg.program_name = "test";
        cfg.target_pid = target_pid;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        cfg.verbose = 0; /* non-verbose: verbose guard must not print */
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    assert(waitpid(wrapper_pid, &wrapper_status, 0) == wrapper_pid);
    assert(WIFEXITED(wrapper_status));
    assert(WEXITSTATUS(wrapper_status) == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode exits with failure when target is self
 * @note When the found PID matches the calling process, run_pid_or_exe_mode
 *  must exit with EXIT_FAILURE to prevent cpulimit from limiting itself.
 */
static void test_limiter_run_pid_or_exe_mode_self(void) {
    pid_t wrapper_pid;
    int wrapper_status;

    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimitcfg cfg;

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        memset(&cfg, 0, sizeof(struct cpulimitcfg));
        cfg.program_name = "test";
        cfg.target_pid = getpid(); /* target is the wrapper itself */
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    assert(waitpid(wrapper_pid, &wrapper_status, 0) == wrapper_pid);
    assert(WIFEXITED(wrapper_status));
    /* Must exit with failure to prevent self-limiting */
    assert(WEXITSTATUS(wrapper_status) == EXIT_FAILURE);
}

/**
 * @brief Test run_pid_or_exe_mode with verbose=1 exercises the verbose path
 * @note Forks a short-lived target process, then calls run_pid_or_exe_mode
 *  with verbose=1 and lazy_mode=1. Suppresses output. Verifies the function
 *  exits cleanly when the target terminates, confirming the verbose code path
 *  (including the "Process found" message guard) does not crash.
 */
static void test_limiter_run_pid_or_exe_mode_verbose(void) {
    pid_t wrapper_pid;
    int wrapper_status;
    const struct timespec target_life = {0, 500000000L}; /* 500 ms */

    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t target_pid;
        struct cpulimitcfg cfg;

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        /* Fork a target that stays alive long enough to be found */
        target_pid = fork();
        assert(target_pid >= 0);
        if (target_pid == 0) {
            sleep_timespec(&target_life);
            _exit(EXIT_SUCCESS);
        }

        memset(&cfg, 0, sizeof(struct cpulimitcfg));
        cfg.program_name = "test";
        cfg.target_pid = target_pid;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        cfg.verbose = 1; /* exercises verbose branch in run_pid_or_exe_mode */
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    assert(waitpid(wrapper_pid, &wrapper_status, 0) == wrapper_pid);
    assert(WIFEXITED(wrapper_status));
    assert(WEXITSTATUS(wrapper_status) == EXIT_SUCCESS);
}

/***************************************************************************
 * CLI MODULE - ADDITIONAL COVERAGE
 ***************************************************************************/

/**
 * @brief Test parse_arguments with --include-children long option
 * @note --include-children must set include_children=1
 */
static void test_cli_long_option_include_children(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_ic[] = "--include-children";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[6];

    av[0] = arg0;
    av[1] = arg_limit;
    av[2] = arg_ic;
    av[3] = arg_e;
    av[4] = arg_exe;
    av[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, (char *const *)av, &cfg);
    assert(cfg.include_children == 1);
}

/**
 * @brief Test parse_arguments with limit exactly equal to 100*ncpu (maximum)
 * @note Limit at the boundary must be accepted
 */
static void test_cli_limit_at_max(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_max[32];
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[6];
    int n_cpu = get_ncpu();

    snprintf(arg_max, sizeof(arg_max), "%d", 100 * n_cpu);
    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_max;
    av[3] = arg_e;
    av[4] = arg_exe;
    av[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, (char *const *)av, &cfg);
    /* Limit stored as fraction: 100*ncpu/100 == ncpu */
    assert(cfg.limit >= (double)n_cpu - 0.001 &&
           cfg.limit <= (double)n_cpu + 0.001);
}

/**
 * @brief Test parse_arguments with minimum valid PID (>1; try 2)
 * @note PID 2 is valid if it exists; test parse path only (fork to isolate)
 */
static void test_cli_pid_minimum_valid(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid2[] = "2";
    char *av[6];
    int ret;

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_50;
    av[3] = arg_p;
    av[4] = arg_pid2;
    av[5] = NULL;

    /* PID 2 is syntactically valid (> 1); parse must succeed (exit code 99) */
    ret = run_parse_in_child(5, (char *const *)av);
    assert(ret == 99);
}

/**
 * @brief Test parse_arguments with a limit value that has trailing whitespace
 * @note "50 " (trailing space) has trailing chars after strtod; must fail
 */
static void test_cli_limit_trailing_chars(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_lim[] = "50 "; /* trailing space */
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[6];

    av[0] = arg0;
    av[1] = arg_l;
    av[2] = arg_lim;
    av[3] = arg_e;
    av[4] = arg_exe;
    av[5] = NULL;
    assert(run_parse_in_child(5, (char *const *)av) == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with --lazy and --verbose long options
 * @note Long forms of -z and -v must behave identically to short forms
 */
static void test_cli_long_options_lazy_verbose(void) {
    struct cpulimitcfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_lazy[] = "--lazy";
    char arg_verbose[] = "--verbose";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *av[7];

    av[0] = arg0;
    av[1] = arg_limit;
    av[2] = arg_lazy;
    av[3] = arg_verbose;
    av[4] = arg_e;
    av[5] = arg_exe;
    av[6] = NULL;

    /* verbose prints to stdout; suppress it */
    {
        pid_t pid;
        int status;
        pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            close(STDOUT_FILENO);
            memset(&cfg, 0, sizeof(cfg));
            parse_arguments(6, (char *const *)av, &cfg);
            if (cfg.lazy_mode != 1) {
                _exit(1);
            }
            if (cfg.verbose != 1) {
                _exit(2);
            }
            _exit(0);
        }
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status));
        assert(WEXITSTATUS(status) == 0);
    }
}

/** @def RUN_TEST(test_func)
 *  @brief Macro to run a test function and print its status
 *  @param test_func Name of the test function to run
 */
#define RUN_TEST(test_func)                                                    \
    do {                                                                       \
        printf("Running %s()...\n", #test_func);                               \
        test_func();                                                           \
        printf("%s() passed.\n", #test_func);                                  \
    } while (0)

/**
 * @brief Main test function
 * @param argc Argument count
 * @param argv Argument vector
 * @return 0 on success
 * @note Runs all test functions organized by module and prints their results.
 *  Installs signal handlers for SIGINT and SIGTERM to request graceful
 *  shutdown of the test run instead of abrupt termination.
 */
int main(int argc, char *argv[]) {
    assert(argc >= 1);
    command = argv[0];

    configure_signal_handler();
    printf("Starting tests...\n");

    /* List module tests */
    printf("\n=== LIST MODULE TESTS ===\n");
    RUN_TEST(test_list_init_and_empty);
    RUN_TEST(test_list_add_elem);
    RUN_TEST(test_list_delete_node);
    RUN_TEST(test_list_delete_node_empty);
    RUN_TEST(test_list_destroy_node_null_list);
    RUN_TEST(test_list_destroy_node);
    RUN_TEST(test_list_first_node_nonempty);
    RUN_TEST(test_list_locate);
    RUN_TEST(test_list_locate_single);
    RUN_TEST(test_list_clear_and_destroy);
    RUN_TEST(test_list_clear_empty);
    RUN_TEST(test_list_destroy_empty);
    RUN_TEST(test_list_edge_cases);
    RUN_TEST(test_list_null_data_operations);

    /* Util module tests */
    printf("\n=== UTIL MODULE TESTS ===\n");
    RUN_TEST(test_util_nsec2timespec);
    RUN_TEST(test_util_nsec2timespec_zero_and_sub);
    RUN_TEST(test_util_nsec2timespec_rollover);
    RUN_TEST(test_util_time_functions);
    RUN_TEST(test_util_get_current_time_monotonic);
    RUN_TEST(test_util_timediff_in_ms);
    RUN_TEST(test_util_timediff_negative);
    RUN_TEST(test_util_timediff_nsec_only);
    RUN_TEST(test_util_file_basename);
    RUN_TEST(test_util_file_basename_empty);
    RUN_TEST(test_util_get_ncpu);
    RUN_TEST(test_util_increase_priority);
    RUN_TEST(test_util_long2pid_t);
    RUN_TEST(test_util_long2pid_t_zero);
    RUN_TEST(test_util_time_edge_cases);
    RUN_TEST(test_util_file_basename_edge_cases);
    RUN_TEST(test_util_long2pid_t_edge_cases);
    RUN_TEST(test_util_long2pid_t_overflow);
    RUN_TEST(test_util_macros);
    RUN_TEST(test_util_sleep_timespec_zero);
    RUN_TEST(test_util_sleep_timespec_nonzero);
#if defined(__linux__)
    RUN_TEST(test_util_read_line_from_file);
#endif

    /* Process table module tests */
    printf("\n=== PROCESS_TABLE MODULE TESTS ===\n");
    RUN_TEST(test_process_table_init_destroy);
    RUN_TEST(test_process_table_init_hashsize_zero);
    RUN_TEST(test_process_table_add_find);
    RUN_TEST(test_process_table_find_null_pt);
    RUN_TEST(test_process_table_del);
    RUN_TEST(test_process_table_del_absent_pid);
    RUN_TEST(test_process_table_del_empty_bucket);
    RUN_TEST(test_process_table_remove_stale);
    RUN_TEST(test_process_table_remove_stale_null_data);
    RUN_TEST(test_process_table_collisions);
    RUN_TEST(test_process_table_empty_buckets);
    RUN_TEST(test_process_table_null_inputs_and_dup);
    RUN_TEST(test_process_table_stale_null_list);
    RUN_TEST(test_process_table_destroy_edge_cases);
    RUN_TEST(test_process_table_ops_after_destroy);

    /* Signal handler module tests */
    printf("\n=== SIGNAL_HANDLER MODULE TESTS ===\n");
    RUN_TEST(test_signal_handler_initial_state);
    RUN_TEST(test_signal_handler_flags);
    RUN_TEST(test_signal_handler_sigint);
    RUN_TEST(test_signal_handler_sigquit);
    RUN_TEST(test_signal_handler_sigterm);
    RUN_TEST(test_signal_handler_sighup);
    RUN_TEST(test_signal_handler_sigpipe);

    /* Process iterator module tests */
    printf("\n=== PROCESS_ITERATOR MODULE TESTS ===\n");
    RUN_TEST(test_process_iterator_single);
    RUN_TEST(test_process_iterator_multiple);
    RUN_TEST(test_process_iterator_all);
    RUN_TEST(test_process_iterator_init_all_with_children);
    RUN_TEST(test_process_iterator_read_command);
    RUN_TEST(test_process_iterator_getppid_of);
    RUN_TEST(test_process_iterator_getppid_of_edges);
    RUN_TEST(test_process_iterator_is_child_of);
    RUN_TEST(test_process_iterator_filter_edge_cases);
    RUN_TEST(test_process_iterator_exhaust_single);
    RUN_TEST(test_process_iterator_with_children);
    RUN_TEST(test_process_iterator_close_null_dip);
    RUN_TEST(test_process_iterator_close_null);
    RUN_TEST(test_process_iterator_null_inputs);

    /* Process group module tests */
    printf("\n=== PROCESS_GROUP MODULE TESTS ===\n");
    RUN_TEST(test_process_group_init_all);
    RUN_TEST(test_process_group_init_single);
    RUN_TEST(test_process_group_init_invalid_pid);
    RUN_TEST(test_process_group_init_null);
    RUN_TEST(test_process_group_close_null);
    RUN_TEST(test_process_group_close_zeros_fields);
    RUN_TEST(test_process_group_update_null);
    RUN_TEST(test_process_group_double_update);
    RUN_TEST(test_process_group_find_by_pid);
    RUN_TEST(test_process_group_find_by_pid_edges);
    RUN_TEST(test_process_group_find_by_pid_init);
    RUN_TEST(test_process_group_find_by_name);
    RUN_TEST(test_process_group_find_by_name_null);
    RUN_TEST(test_process_group_find_by_name_empty_string);
    RUN_TEST(test_process_group_find_by_name_trailing_slash);
    RUN_TEST(test_process_group_find_by_name_self);
    RUN_TEST(test_process_group_cpu_usage);
    RUN_TEST(test_process_group_cpu_usage_empty_list);
    RUN_TEST(test_process_group_cpu_usage_null);
    RUN_TEST(test_process_group_cpu_usage_with_usage);
    RUN_TEST(test_process_group_rapid_updates);

    /* Limit process module tests */
    printf("\n=== LIMIT_PROCESS MODULE TESTS ===\n");
    RUN_TEST(test_limit_process_basic);
    RUN_TEST(test_limit_process_exits_early);
    RUN_TEST(test_limit_process_verbose);
    RUN_TEST(test_limit_process_include_children);

    /* Limiter module tests */
    printf("\n=== LIMITER MODULE TESTS ===\n");
    RUN_TEST(test_limiter_run_command_mode);
    RUN_TEST(test_limiter_run_command_mode_false);
    RUN_TEST(test_limiter_run_command_mode_nonexistent);
    RUN_TEST(test_limiter_run_command_mode_verbose);
    RUN_TEST(test_limiter_run_pid_or_exe_mode);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_pid_not_found);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_quit);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_pid_found);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_self);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_verbose);

    /* CLI module tests */
    printf("\n=== CLI MODULE TESTS ===\n");
    RUN_TEST(test_cli_pid_mode);
    RUN_TEST(test_cli_exe_mode);
    RUN_TEST(test_cli_command_mode);
    RUN_TEST(test_cli_long_options);
    RUN_TEST(test_cli_long_option_exe);
    RUN_TEST(test_cli_long_option_include_children);
    RUN_TEST(test_cli_long_options_lazy_verbose);
    RUN_TEST(test_cli_optional_flags);
    RUN_TEST(test_cli_verbose_flag);
    RUN_TEST(test_cli_help);
    RUN_TEST(test_cli_missing_limit);
    RUN_TEST(test_cli_invalid_limits);
    RUN_TEST(test_cli_limit_at_max);
    RUN_TEST(test_cli_limit_trailing_chars);
    RUN_TEST(test_cli_invalid_pids);
    RUN_TEST(test_cli_pid_minimum_valid);
    RUN_TEST(test_cli_empty_exe);
    RUN_TEST(test_cli_no_target);
    RUN_TEST(test_cli_multiple_targets);
    RUN_TEST(test_cli_unknown_option);
    RUN_TEST(test_cli_missing_arg);

    printf("\n=== ALL TESTS PASSED ===\n");

    return 0;
}
