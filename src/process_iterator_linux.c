/**
 *
 * cpulimit - a CPU limiter for Linux
 *
 * Copyright (C) 2005-2012, by:  Angelo Marletta <angelo dot marletta at gmail dot com>
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
 */

#ifdef __linux__

#ifndef __PROCESS_ITERATOR_LINUX_C
#define __PROCESS_ITERATOR_LINUX_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "process_iterator.h"

int init_process_iterator(struct process_iterator *it, struct process_filter *filter)
{
    it->filter = filter;
    it->end_of_processes = 0;
    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        /* In this case, it->dip is never used */
        it->dip = NULL;
        return 0;
    }
    /* open a directory stream to /proc directory */
    if ((it->dip = opendir("/proc")) == NULL)
    {
        perror("opendir");
        return -1;
    }
    return 0;
}

static int read_process_info(pid_t pid, struct process *p)
{
    char statfile[64], exefile[64], state;
    const size_t buff_size = 2048;
    ssize_t nread;
    char *buffer;
    double usertime, systime;
    long ppid;
    int fd;
    static long sc_clk_tck = -1;

    p->pid = pid;

    /* read stat file */
    sprintf(statfile, "/proc/%ld/stat", (long)p->pid);
    if ((fd = open(statfile, O_RDONLY)) < 0)
    {
        return -1;
    }
    buffer = (char *)malloc(buff_size);
    if (buffer == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the buffer\n");
        close(fd);
        exit(EXIT_FAILURE);
    }
    nread = read(fd, buffer, buff_size - 1);
    close(fd);
    if (nread <= 0)
    {
        free(buffer);
        return -1;
    }
    buffer[nread] = '\0';
    if (sscanf(buffer, "%*s (%*[^)]) %c %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %lf %lf",
               &state, &ppid, &usertime, &systime) != 4 ||
        strchr("ZXx", state) != NULL)
    {
        free(buffer);
        return -1;
    }
    free(buffer);
    p->ppid = (pid_t)ppid;
    if (sc_clk_tck < 0)
    {
        sc_clk_tck = sysconf(_SC_CLK_TCK);
    }
    p->cputime = (usertime + systime) * 1000.0 / (double)sc_clk_tck;

    /* read command line */
    sprintf(exefile, "/proc/%ld/cmdline", (long)p->pid);
    if ((fd = open(exefile, O_RDONLY)) < 0)
    {
        return -1;
    }
    nread = read(fd, p->command, sizeof(p->command) - 1);
    close(fd);
    if (nread <= 0)
    {
        return -1;
    }
    p->command[nread] = '\0';

    return 0;
}

pid_t getppid_of(pid_t pid)
{
    char statfile[64], state;
    const size_t buff_size = 2048;
    ssize_t nread;
    char *buffer;
    long ppid;
    int fd;

    /* read stat file */
    sprintf(statfile, "/proc/%ld/stat", (long)pid);
    if ((fd = open(statfile, O_RDONLY)) < 0)
    {
        return (pid_t)-1;
    }
    buffer = (char *)malloc(buff_size);
    if (buffer == NULL)
    {
        fprintf(stderr, "Memory allocation failed for the buffer\n");
        close(fd);
        exit(EXIT_FAILURE);
    }
    nread = read(fd, buffer, buff_size - 1);
    close(fd);
    if (nread <= 0)
    {
        free(buffer);
        return (pid_t)-1;
    }
    buffer[nread] = '\0';
    if (sscanf(buffer, "%*s (%*[^)]) %c %ld", &state, &ppid) != 2 ||
        strchr("ZXx", state) != NULL)
    {
        free(buffer);
        return (pid_t)-1;
    }
    free(buffer);
    return (pid_t)ppid;
}

static int get_start_time(pid_t pid, struct timespec *start_time)
{
    struct stat procfs_stat;
    char procfs_path[64];
    int ret;
    if (start_time == NULL)
        return -1;
    sprintf(procfs_path, "/proc/%ld", (long)pid);
    if ((ret = stat(procfs_path, &procfs_stat)) == 0)
        *start_time = procfs_stat.st_ctim;
    return ret;
}

/* int earlier_than(const struct timespec *t1, const struct timespec *t2) */
#define earlier_than(t1, t2)        \
    ((t1)->tv_sec < (t2)->tv_sec || \
     ((t1)->tv_sec == (t2)->tv_sec && (t1)->tv_nsec < (t2)->tv_nsec))

int is_child_of(pid_t child_pid, pid_t parent_pid)
{
    int ret_child, ret_parent;
    struct timespec child_start_time, parent_start_time;
    if (child_pid <= 1 || parent_pid <= 0 || child_pid == parent_pid)
        return 0;
    if (parent_pid == 1)
        return 1;
    ret_parent = get_start_time(parent_pid, &parent_start_time);
    while (child_pid > 1)
    {
        if (ret_parent == 0)
        {
            ret_child = get_start_time(child_pid, &child_start_time);
            if (ret_child == 0 && earlier_than(&child_start_time, &parent_start_time))
                return 0;
        }
        child_pid = getppid_of(child_pid);
        if (child_pid == parent_pid)
            return 1;
    }
    return 0;
}

static int is_numeric(const char *str)
{
    if (str == NULL || *str == '\0')
        return 0;
    for (; *str != '\0'; str++)
    {
        if (!isdigit(*str))
            return 0;
    }
    return 1;
}

int get_next_process(struct process_iterator *it, struct process *p)
{
    const struct dirent *dit = NULL;

    if (it->end_of_processes)
    {
        return -1;
    }

    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        int ret = read_process_info(it->filter->pid, p);
        it->end_of_processes = 1;
        return ret == 0 ? 0 : -1;
    }

    /* read in from /proc and seek for process dirs */
    while ((dit = readdir(it->dip)) != NULL)
    {
#ifdef _DIRENT_HAVE_D_TYPE
        if (dit->d_type != DT_DIR)
            continue;
#endif
        if (!is_numeric(dit->d_name) ||
            (p->pid = (pid_t)atol(dit->d_name)) <= 0)
            continue;
        if (it->filter->pid != 0 &&
            it->filter->pid != p->pid &&
            !is_child_of(p->pid, it->filter->pid))
            continue;
        if (read_process_info(p->pid, p) != 0)
            continue;
        return 0;
    }
    /* end of processes */
    it->end_of_processes = 1;
    return -1;
}

int close_process_iterator(struct process_iterator *it)
{
    int ret = 0;
    if (it == NULL)
        return -1; /* Invalid argument */

    if (it->dip != NULL)
    {
        if ((ret = closedir(it->dip)) != 0)
        {
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
