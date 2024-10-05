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

#include <sys/vfs.h>
#include <linux/magic.h>
#include "process_iterator.h"
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

static int check_proc(void)
{
    struct statfs mnt;
    if (statfs("/proc", &mnt) < 0)
        return 0;
    if (mnt.f_type != PROC_SUPER_MAGIC)
        return 0;
    return 1;
}

int init_process_iterator(struct process_iterator *it, struct process_filter *filter)
{
    if (!check_proc())
    {
        fprintf(stderr, "procfs is not mounted!\nAborting\n");
        exit(-2);
    }
    /* open a directory stream to /proc directory */
    if ((it->dip = opendir("/proc")) == NULL)
    {
        perror("opendir");
        return -1;
    }
    it->filter = filter;
    return 0;
}

static int read_process_info(pid_t pid, struct process *p)
{
    char statfile[32], exefile[32], state;
    double utime, stime;
    long ppid;
    FILE *fd;
    static double sc_clk_tck = -1.0;

    p->pid = pid;

    /* read command line */
    sprintf(exefile, "/proc/%ld/cmdline", (long)p->pid);
    if ((fd = fopen(exefile, "r")) == NULL)
    {
        return -1;
    }
    if (fgets(p->command, sizeof(p->command), fd) == NULL)
    {
        fclose(fd);
        return -1;
    }
    fclose(fd);

    /* read stat file */
    sprintf(statfile, "/proc/%ld/stat", (long)p->pid);
    if ((fd = fopen(statfile, "r")) == NULL)
    {
        return -1;
    }
    if (fscanf(fd, "%*d (%*[^)]) %c %ld %*d %*d %*d %*d %*d %*d %*d %*d %*d %lf %lf",
               &state, &ppid, &utime, &stime) != 4 ||
        strchr("ZXx", state) != NULL)
    {
        fclose(fd);
        return -1;
    }
    fclose(fd);
    p->ppid = (pid_t)ppid;
    if (sc_clk_tck < 0)
    {
        sc_clk_tck = (double)sysconf(_SC_CLK_TCK);
    }
    p->cputime = (utime + stime) * 1000.0 / sc_clk_tck;

    return 0;
}

pid_t getppid_of(pid_t pid)
{
    char statfile[32];
    FILE *fd;
    long ppid = -1;
    if (pid <= 0)
        return (pid_t)(-1);
    sprintf(statfile, "/proc/%ld/stat", (long)pid);
    if ((fd = fopen(statfile, "r")) != NULL)
    {
        if (fscanf(fd, "%*d (%*[^)]) %*c %ld", &ppid) != 1)
            ppid = -1;
        fclose(fd);
    }
    return (pid_t)ppid;
}

static int get_start_time(pid_t pid, struct timespec *start_time)
{
    struct stat procfs_stat;
    char procfs_path[32];
    int ret;
    sprintf(procfs_path, "/proc/%ld", (long)pid);
    ret = stat(procfs_path, &procfs_stat);
    if (ret == 0 && start_time != NULL)
        *start_time = procfs_stat.st_ctim;
    return ret;
}

static int earlier_than(const struct timespec *t1, const struct timespec *t2)
{
    return t1->tv_sec < t2->tv_sec ||
           (t1->tv_sec == t2->tv_sec && t1->tv_nsec < t2->tv_nsec);
}

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
    struct dirent *dit = NULL;

    if (it->dip == NULL)
    {
        /* end of processes */
        return -1;
    }
    if (it->filter->pid != 0 && !it->filter->include_children)
    {
        int ret = read_process_info(it->filter->pid, p);
        closedir(it->dip);
        it->dip = NULL;
        if (ret != 0)
            return -1;
        return 0;
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
    closedir(it->dip);
    it->dip = NULL;
    return -1;
}

int close_process_iterator(struct process_iterator *it)
{
    if (it->dip != NULL && closedir(it->dip) == -1)
    {
        perror("closedir");
        return 1;
    }
    it->dip = NULL;
    return 0;
}

#endif
#endif
