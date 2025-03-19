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

#include "util.h" /* Must be included at first!!! */
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#if !defined(CLOCK_MONOTONIC) && !defined(CLOCK_REALTIME)
#include <sys/time.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <fcntl.h>
#include <stdlib.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#ifdef __IMPL_GETLOADAVG
#include <stdlib.h>
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) && !defined(_SC_NPROCESSORS_ONLN)
#include <sys/sysinfo.h>
#endif

void nsec2timespec(double nsec, struct timespec *t)
{
    t->tv_sec = (time_t)(nsec / 1e9);
    t->tv_nsec = (long)(nsec - (double)t->tv_sec * 1e9);
}

int get_current_time(struct timespec *ts)
{
#if defined(CLOCK_MONOTONIC)
    return clock_gettime(CLOCK_MONOTONIC, ts);
#elif defined(CLOCK_REALTIME)
    return clock_gettime(CLOCK_REALTIME, ts);
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
    {
        return -1;
    }
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
    return 0;
#endif
}

int sleep_timespec(const struct timespec *t)
{
#if (defined(__linux__) || defined(__FreeBSD__)) &&           \
    defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L && \
    defined(CLOCK_MONOTONIC)
    return clock_nanosleep(CLOCK_MONOTONIC, 0, t, NULL);
#else
    return nanosleep(t, NULL);
#endif
}

double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier)
{
    return (double)(later->tv_sec - earlier->tv_sec) * 1e3 +
           (double)(later->tv_nsec - earlier->tv_nsec) / 1e6;
}

const char *file_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p != NULL ? p + 1 : path;
}

void increase_priority(void)
{
    static const int MAX_PRIORITY = -20;
    int old_priority, priority;
    old_priority = getpriority(PRIO_PROCESS, 0);
    for (priority = MAX_PRIORITY; priority < old_priority; priority++)
    {
        if (setpriority(PRIO_PROCESS, 0, priority) == 0 &&
            getpriority(PRIO_PROCESS, 0) == priority)
            break;
    }
}

int get_ncpu(void)
{
    static int cached_ncpu = -1;

    if (cached_ncpu > 0)
    {
        return cached_ncpu;
    }

#if defined(_SC_NPROCESSORS_ONLN)
    {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cached_ncpu = ncpu > 0 ? (int)ncpu : 1;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    {
        int ncpu = 0, mib[2];
        mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        mib[1] = HW_AVAILCPU;
#elif defined(HW_NCPU)
        mib[1] = HW_NCPU;
#else
#error "Unsupported platform"
#endif
        size_t len = sizeof(ncpu);
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1)
        {
            mib[1] = HW_NCPU;
            if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1)
            {
                cached_ncpu = 1;
                return 1;
            }
        }
        cached_ncpu = ncpu;
    }
#elif defined(__linux__)
    {
        int ncpu = get_nprocs();
        cached_ncpu = ncpu > 0 ? ncpu : 1;
    }
#else
#error "Unsupported platform"
#endif

    return cached_ncpu;
}

pid_t get_pid_max(void)
{
#if defined(__linux__)
    int fd;
    char buffer[64];
    ssize_t bytes_read;
    long pid_max;

#ifdef O_CLOEXEC
    if ((fd = open("/proc/sys/kernel/pid_max", O_RDONLY | O_CLOEXEC)) < 0)
#else
    if ((fd = open("/proc/sys/kernel/pid_max", O_RDONLY)) < 0)
#endif
    {
        fprintf(stderr, "Failed to open /proc/sys/kernel/pid_max\n");
        return PID_T_MAX;
    }

    bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (bytes_read <= 0)
    {
        fprintf(stderr, "Failed to read /proc/sys/kernel/pid_max\n");
        return PID_T_MAX;
    }
    buffer[bytes_read] = '\0';

    pid_max = strtol(buffer, NULL, 10);
    if (pid_max <= 0)
    {
        fprintf(stderr, "Failed to read /proc/sys/kernel/pid_max\n");
        return PID_T_MAX;
    }

    return (pid_t)pid_max;
#elif defined(__FreeBSD__)
    return (pid_t)99998;
#elif defined(__APPLE__)
    return (pid_t)99998;
#else
#error "Unsupported platform"
#endif
}

#ifdef __IMPL_GETLOADAVG
int __getloadavg(double *loadavg, int nelem)
{
    struct sysinfo si;
    int i;

    if (nelem <= 0)
        return nelem ? -1 : 0;
    sysinfo(&si);
    if (nelem > 3)
        nelem = 3;

    for (i = 0; i < nelem; i++)
    {
        loadavg[i] = 1.0 / (1 << SI_LOAD_SHIFT) * si.loads[i];
    }

    return nelem;
}
#endif
