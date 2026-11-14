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

#ifndef CPULIMIT_PROCESS_ITERATOR_LINUX_C
#define CPULIMIT_PROCESS_ITERATOR_LINUX_C

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "file_io.h"
#include "process_iterator.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @brief Initialize a process iterator with the given filter.
 *
 * @param iter Iterator to initialize.
 * @param filter Filter criteria, which must stay valid during iteration.
 * @return 0 on success, -1 on a NULL argument, an allocation failure or a
 *         backend-specific initialization error; never calls exit().
 *
 * @note The filter pointer is stored until close_process_iterator().
 */
int init_process_iterator(struct process_iterator *iter,
                          const struct process_filter *filter) {
    if (iter == NULL || filter == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->filter = filter;
    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        /* A single PID without children needs no /proc scan. */
        iter->proc_dir = NULL;
        return 0;
    }
    iter->proc_dir = opendir("/proc");
    if (iter->proc_dir == NULL) {
        perror("opendir");
        close_process_iterator(iter);
        return -1;
    }
    return 0;
}

/**
 * @brief Read information for one PID into the process structure.
 *
 * @param pid PID to query.
 * @param proc Structure to populate.
 * @param read_cmd Non-zero to read argv[0].
 * @return 0 on success, -1 on failure.
 */
static int read_process_info(pid_t pid, struct process *proc, int read_cmd) {
    char statfile[64], cmdline_path[64];
    char *buffer;
    const char *p;
    char state;
    long ppid;
    double user_time, sys_time;
    double start_time = UNKNOWN_START_TIME;
    int parsed;
    size_t cmd_len;
    /* -1: not looked up; -2: a failed lookup is latched. */
    static long sc_clk_tck = -1;

    memset(proc, 0, sizeof(struct process));
    proc->pid = pid;

    if (snprintf(statfile, sizeof(statfile), "/proc/%ld/stat", (long)pid) >=
        (int)sizeof(statfile)) {
        return -1;
    }
    buffer = read_file_contents(statfile);
    if (buffer == NULL) {
        return -1;
    }
    /* The last ')' ends comm, which may contain parentheses or newlines. */
    p = strrchr(buffer, ')');
    if (p == NULL) {
        free(buffer);
        return -1;
    }
    /* starttime is optional so a truncated stat line stays usable. */
    parsed = sscanf(
        p + 1,
        " %c %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %lf %lf %*s %*s %*s %*s %*s %*s %lf",
        &state, &ppid, &user_time, &sys_time, &start_time);
    if ((parsed != 4 && parsed != 5) || !isalpha((unsigned char)state) ||
        strchr("ZXx", state) != NULL || ppid <= 0 || user_time < 0 ||
        sys_time < 0) {
        free(buffer);
        return -1;
    }
    free(buffer);
    proc->ppid = long_to_pid_t(ppid);
    if (proc->ppid < 0) {
        return -1;
    }
    if (sc_clk_tck < 0) {
        /* Latch a failed clock lookup to avoid retrying it per process. */
        if (sc_clk_tck == -2) {
            return -1;
        }
        errno = 0;
        sc_clk_tck = sysconf(_SC_CLK_TCK);
        if (sc_clk_tck <= 0) {
            /* Keep the cache negative to avoid a zero divisor. */
            if (errno != 0) {
                perror("sysconf(_SC_CLK_TCK)");
            } else {
                fprintf(stderr, "sysconf(_SC_CLK_TCK) returned %ld\n",
                        sc_clk_tck);
            }
            sc_clk_tck = -2;
            return -1;
        }
    }
    proc->cpu_time = (user_time + sys_time) * 1000.0 / (double)sc_clk_tck;
    proc->start_time =
        (parsed == 5) ? start_time / (double)sc_clk_tck : UNKNOWN_START_TIME;

    if (!read_cmd) {
        return 0;
    }
    /* /proc/[pid]/cmdline is NUL-separated; copy only argv[0]. */
    if (snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%ld/cmdline",
                 (long)pid) >= (int)sizeof(cmdline_path)) {
        return -1;
    }
    buffer = read_file_contents(cmdline_path);
    if (buffer == NULL) {
        return -1;
    }
    /* Copy only the bytes present; proc was zeroed at entry. */
    cmd_len = strlen(buffer);
    if (cmd_len > sizeof(proc->command) - 1) {
        cmd_len = sizeof(proc->command) - 1;
    }
    memcpy(proc->command, buffer, cmd_len);
    /* Terminate explicitly: a full buffer has no copied NUL. */
    proc->command[cmd_len] = '\0';
    free(buffer);
    /* Reject an empty argv[0]. */
    if (proc->command[0] == '\0') {
        return -1;
    }
    return 0;
}

/**
 * @brief Read the parent PID of a process.
 *
 * @param pid PID to query.
 * @return The parent PID, or -1 when the process is gone, a zombie, or the
 *         platform lookup fails.
 *
 * @note getppid_of(1) differs: Linux returns -1, FreeBSD 0, macOS 0 or -1.
 */
pid_t getppid_of(pid_t pid) {
    char statfile[64];
    char *buffer;
    const char *p;
    char state;
    long ppid;

    if (snprintf(statfile, sizeof(statfile), "/proc/%ld/stat", (long)pid) >=
        (int)sizeof(statfile)) {
        return (pid_t)-1;
    }
    buffer = read_file_contents(statfile);
    if (buffer == NULL) {
        return (pid_t)-1;
    }
    p = strrchr(buffer, ')');
    if (p == NULL) {
        free(buffer);
        return (pid_t)-1;
    }
    if (sscanf(p + 1, " %c %ld", &state, &ppid) != 2 ||
        !isalpha((unsigned char)state) || strchr("ZXx", state) != NULL ||
        ppid <= 0) {
        free(buffer);
        return (pid_t)-1;
    }
    free(buffer);
    return long_to_pid_t(ppid);
}

/**
 * @brief Report whether a process is currently stopped.
 *
 * @param pid PID to query.
 * @return 1 when stopped, 0 when running, gone or unreadable.
 *
 * @note Read-only: sends no signal and starts no scan.
 */
int process_is_stopped(pid_t pid) {
    char statfile[64];
    char *buffer;
    const char *p;
    char state;

    if (snprintf(statfile, sizeof(statfile), "/proc/%ld/stat", (long)pid) >=
        (int)sizeof(statfile)) {
        return 0;
    }
    buffer = read_file_contents(statfile);
    if (buffer == NULL) {
        return 0;
    }
    p = strrchr(buffer, ')');
    if (p == NULL) {
        free(buffer);
        return 0;
    }
    if (sscanf(p + 1, " %c", &state) != 1 || !isalpha((unsigned char)state)) {
        free(buffer);
        return 0;
    }
    free(buffer);
    return state == 'T' || state == 't';
}

/**
 * @brief Read one process's parent for is_child_of_via().
 *
 * @param context Unused.
 * @param pid PID whose parent is wanted.
 * @return The parent PID, or -1 on failure.
 */
static pid_t is_child_lookup(void *context, pid_t pid) {
    (void)context;
    return getppid_of(pid);
}

/**
 * @brief Determine whether one process descends from another.
 *
 * @param child_pid PID to test.
 * @param parent_pid PID of the potential ancestor.
 * @return 1 when parent_pid is in the ancestry chain, 0 otherwise.
 *
 * Walks the shared parent chain with the platform's lookup. Returns 0 for
 * an invalid or self pair, a chain longer than IS_CHILD_MAX_DEPTH, or two
 * consecutive lookup failures.
 */
int is_child_of(pid_t child_pid, pid_t parent_pid) {
    return is_child_of_via(is_child_lookup, NULL, child_pid, parent_pid);
}

/**
 * @brief Retrieve the next process matching the filter.
 *
 * @param iter Iterator to advance.
 * @param proc Structure to populate.
 * @return 0 with data in proc, -1 when the scan ends or an argument is
 *         NULL; the Linux backend also returns PROCESS_ITER_ERROR when a
 *         scan stops early on an error.
 *
 * Always populates pid, ppid, cpu_time and start_time; command is filled
 * only when filter->read_cmd is set. Zombies and system processes are
 * skipped where the platform can identify them.
 */
int get_next_process(struct process_iterator *iter, struct process *proc) {
    const struct dirent *dir_entry = NULL;

    if (iter == NULL || proc == NULL || iter->filter == NULL) {
        return -1;
    }
    if (iter->end_of_processes) {
        return -1;
    }

    if (iter->filter->pid != 0 && !iter->filter->include_children) {
        int ret =
            read_process_info(iter->filter->pid, proc, iter->filter->read_cmd);
        iter->end_of_processes = 1;
        return ret == 0 ? 0 : -1;
    }

    if (iter->proc_dir == NULL) {
        iter->end_of_processes = 1;
        return -1;
    }

    while (1) {
        pid_t pid;
        char *endptr;
        long long_pid;
        /* readdir() reports EOF and failure alike; clear errno first. */
        errno = 0;
        dir_entry = readdir(iter->proc_dir);
        if (dir_entry == NULL) {
            if (errno != 0) {
                /* errno still carries the readdir() failure. */
                iter->end_of_processes = 1;
                return PROCESS_ITER_ERROR;
            }
            break;
        }
#ifdef _DIRENT_HAVE_D_TYPE
        /* d_type needs a numeric-name check for DT_UNKNOWN. */
        if (dir_entry->d_type != DT_DIR && dir_entry->d_type != DT_UNKNOWN) {
            continue;
        }
#endif
        errno = 0;
        long_pid = strtol(dir_entry->d_name, &endptr, 10);
        if (errno != 0 || endptr == dir_entry->d_name || *endptr != '\0') {
            continue;
        }
        pid = long_to_pid_t(long_pid);
        if (pid <= 0) {
            continue;
        }
        if (!process_matches_filter(pid, iter->filter)) {
            continue;
        }
        if (read_process_info(pid, proc, iter->filter->read_cmd) != 0) {
            continue;
        }
        return 0;
    }
    iter->end_of_processes = 1;
    return -1;
}

/**
 * @brief Release the iterator's platform-specific resources.
 *
 * @param iter Iterator to close.
 * @return 0 on success, -1 on a close failure; macOS has none to fail.
 *
 * @note Re-initialize the iterator before using it again.
 */
int close_process_iterator(struct process_iterator *iter) {
    int ret = 0;
    if (iter == NULL) {
        return -1;
    }

    if (iter->proc_dir != NULL) {
        ret = closedir(iter->proc_dir);
        if (ret != 0) {
            perror("closedir");
        }
    }
    memset(iter, 0, sizeof(*iter));
    return ret;
}

#endif
#else
/* Keep the translation unit non-empty on other platforms. */
typedef int cpulimit_process_iterator_linux_placeholder;
#endif
