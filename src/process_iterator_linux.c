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

#ifdef __linux__

#ifndef __PROCESS_ITERATOR_LINUX_C
#define __PROCESS_ITERATOR_LINUX_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_iterator.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Initialize a process iterator
 * @param it Pointer to the process_iterator structure
 * @param filter Pointer to the process_filter structure
 * @return 0 on success, -1 on failure
 */
int init_process_iterator(struct process_iterator *it,
                          const struct process_filter *filter) {
    it->filter = filter;
    it->end_of_processes = 0;
    if (it->filter->pid != 0 && !it->filter->include_children) {
        /* In this case, it->dip is never used */
        it->dip = NULL;
        return 0;
    }
    /* Open a directory stream to /proc directory */
    if ((it->dip = opendir("/proc")) == NULL) {
        perror("opendir");
        return -1;
    }
    return 0;
}

/**
 * @brief Read process information from /proc filesystem
 * @param pid Process ID to query
 * @param p Pointer to process structure to fill
 * @param read_cmd Flag indicating whether to read command line
 * @return 0 on success, -1 on failure
 */
static int read_process_info(pid_t pid, struct process *p, int read_cmd) {
    char statfile[64], exefile[64], state;
    char *buffer;
    const char *ptr_start;
    double usertime, systime;
    long ppid;
    static long sc_clk_tck = -1;

    memset(p, 0, sizeof(struct process));
    p->pid = pid;

    /* Read stat file */
    sprintf(statfile, "/proc/%ld/stat", (long)p->pid);
    if ((buffer = read_line_from_file(statfile)) == NULL) {
        return -1;
    }
    ptr_start = strrchr(buffer, ')');
    if (ptr_start == NULL) {
        free(buffer);
        return -1;
    }
    if (sscanf(ptr_start,
               ") %c %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %lf %lf", &state,
               &ppid, &usertime, &systime) != 4 ||
        !isalpha(state) || strchr("ZXx", state) != NULL || ppid <= 0 ||
        usertime < 0 || systime < 0) {
        free(buffer);
        return -1;
    }
    free(buffer);
    p->ppid = (pid_t)ppid;
    if (sc_clk_tck < 0) {
        sc_clk_tck = sysconf(_SC_CLK_TCK);
        /* Check sysconf result */
        if (sc_clk_tck <= 0) {
            fprintf(stderr, "sysconf(_SC_CLK_TCK) failed\n");
            exit(EXIT_FAILURE);
        }
    }
    p->cputime = (usertime + systime) * 1000.0 / (double)sc_clk_tck;

    if (!read_cmd) {
        return 0;
    }
    /* Read command line */
    sprintf(exefile, "/proc/%ld/cmdline", (long)p->pid);
    if ((buffer = read_line_from_file(exefile)) == NULL) {
        return -1;
    }
    strncpy(p->command, buffer, sizeof(p->command) - 1);
    p->command[sizeof(p->command) - 1] = '\0';
    free(buffer);
    return 0;
}

/**
 * @brief Get the parent process ID (PPID) of a given PID
 * @param pid The given PID
 * @return Parent process ID, or -1 on error
 */
pid_t getppid_of(pid_t pid) {
    char statfile[64], state;
    char *buffer;
    const char *ptr_start;
    long ppid;

    /* Read stat file */
    sprintf(statfile, "/proc/%ld/stat", (long)pid);
    if ((buffer = read_line_from_file(statfile)) == NULL) {
        return (pid_t)-1;
    }
    ptr_start = strrchr(buffer, ')');
    if (ptr_start == NULL) {
        free(buffer);
        return (pid_t)-1;
    }
    if (sscanf(ptr_start, ") %c %ld", &state, &ppid) != 2 || !isalpha(state) ||
        strchr("ZXx", state) != NULL || ppid <= 0) {
        free(buffer);
        return (pid_t)-1;
    }
    free(buffer);
    return (pid_t)ppid;
}

/**
 * @brief Get the start time of a process
 * @param pid Process ID to query
 * @param start_time Pointer to timespec to store start time
 * @return 0 on success, -1 on failure
 */
static int get_start_time(pid_t pid, struct timespec *start_time) {
    struct stat procfs_stat;
    char procfs_path[64];
    int ret;
    if (start_time == NULL) {
        return -1;
    }
    sprintf(procfs_path, "/proc/%ld", (long)pid);
    if ((ret = stat(procfs_path, &procfs_stat)) == 0) {
#if (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L) ||                \
    (defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 700)
        *start_time = procfs_stat.st_mtim;
#else
        start_time->tv_sec = procfs_stat.st_mtime;
        start_time->tv_nsec = 0;
#endif
    }
    return ret;
}

/**
 * @brief Compare two timespec structures to see if t1 is earlier than t2
 * @param t1 First timespec
 * @param t2 Second timespec
 * @return 1 if t1 is earlier than t2, 0 otherwise
 */
static int earlier_than(const struct timespec *t1, const struct timespec *t2) {
    return t1->tv_sec < t2->tv_sec ||
           (t1->tv_sec == t2->tv_sec && t1->tv_nsec < t2->tv_nsec);
}

/**
 * @brief Check if a process is a child of another process
 * @param child_pid Potential child process ID
 * @param parent_pid Potential parent process ID
 * @return 1 if child_pid is a child of parent_pid, 0 otherwise
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    int ret_child, ret_parent;
    struct timespec child_start_time, parent_start_time;
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid) {
        return 0;
    }
    if (parent_pid == 1) {
        return 1;
    }
    ret_parent = get_start_time(parent_pid, &parent_start_time);
    while (child_pid > 1) {
        if (ret_parent == 0) {
            ret_child = get_start_time(child_pid, &child_start_time);
            if (ret_child == 0 &&
                earlier_than(&child_start_time, &parent_start_time)) {
                return 0;
            }
        }
        child_pid = getppid_of(child_pid);
        if (child_pid < 0) {
            return 0;
        }
        if (child_pid == parent_pid) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Check if a string consists only of digits
 * @param str String to check
 * @return 1 if the string is numeric, 0 otherwise
 */
static int is_numeric(const char *str) {
    if (str == NULL || *str == '\0') {
        return 0;
    }
    for (; *str != '\0'; str++) {
        if (!isdigit(*str)) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Get the next process matching the filter criteria
 * @param it Pointer to the process_iterator structure
 * @param p Pointer to the process structure to store process information
 * @return 0 on success, -1 if no more processes are available
 */
int get_next_process(struct process_iterator *it, struct process *p) {
    const struct dirent *dit = NULL;

    if (it->end_of_processes) {
        return -1;
    }

    if (it->filter->pid != 0 && !it->filter->include_children) {
        int ret = read_process_info(it->filter->pid, p, it->filter->read_cmd);
        it->end_of_processes = 1;
        return ret == 0 ? 0 : -1;
    }

    /* Read in from /proc and seek for process directories */
    while ((dit = readdir(it->dip)) != NULL) {
        pid_t pid;
#ifdef _DIRENT_HAVE_D_TYPE
        if (dit->d_type != DT_DIR && dit->d_type != DT_UNKNOWN) {
            continue;
        }
#endif
        if (!is_numeric(dit->d_name) || (pid = (pid_t)atol(dit->d_name)) <= 0) {
            continue;
        }
        if (it->filter->pid != 0 && it->filter->pid != pid &&
            !is_child_of(pid, it->filter->pid)) {
            continue;
        }
        if (read_process_info(pid, p, it->filter->read_cmd) != 0) {
            continue;
        }
        return 0;
    }
    /* End of processes */
    it->end_of_processes = 1;
    return -1;
}

/**
 * @brief Close the process iterator and free resources
 * @param it Pointer to the process_iterator structure
 * @return 0 on success, -1 on failure
 */
int close_process_iterator(struct process_iterator *it) {
    int ret = 0;
    if (it == NULL) {
        return -1; /* Invalid argument */
    }

    if (it->dip != NULL) {
        if ((ret = closedir(it->dip)) != 0) {
            perror("closedir");
        }
        it->dip = NULL;
    }

    it->end_of_processes = 0;
    it->filter = NULL;

    return ret == 0 ? 0 : -1;
}

#endif
#endif
