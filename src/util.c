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

#include "util.h" /* Must be included at first!!! */

#include <string.h>
#include <sys/resource.h>
#if !defined(CLOCK_MONOTONIC) && !defined(CLOCK_REALTIME)
#include <sys/time.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stddef.h>
#include <sys/sysctl.h>
#endif

#ifdef __IMPL_GETLOADAVG
#include <stdlib.h>
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) && !defined(_SC_NPROCESSORS_ONLN)
#include <sys/sysinfo.h>
#endif

/**
 * @brief Convert nanoseconds to a timespec structure
 * @param nsec Number of nanoseconds to convert
 * @param t Pointer to a timespec structure where the converted time
 *          will be stored
 */
void nsec2timespec(double nsec, struct timespec *t) {
    t->tv_sec = (time_t)(nsec / 1e9);
    t->tv_nsec = (long)(nsec - (double)t->tv_sec * 1e9);
}

/**
 * @brief Get the current time
 * @param ts Pointer to timespec structure to store the current time
 * @return 0 on success, -1 on failure
 */
int get_current_time(struct timespec *ts) {
#if defined(CLOCK_MONOTONIC)
    return clock_gettime(CLOCK_MONOTONIC, ts);
#elif defined(CLOCK_REALTIME)
    return clock_gettime(CLOCK_REALTIME, ts);
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL)) {
        return -1;
    }
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
    return 0;
#endif
}

/**
 * @brief Sleep for a specified timespec duration
 * @param t Pointer to a timespec structure that specifies the duration
 *          to sleep
 * @return 0 for success, or -1 for failure
 */
int sleep_timespec(const struct timespec *t) {
#if (defined(__linux__) || defined(__FreeBSD__)) &&                            \
    defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(CLOCK_MONOTONIC)
    return clock_nanosleep(CLOCK_MONOTONIC, 0, t, NULL);
#else
    return nanosleep(t, NULL);
#endif
}

/**
 * @brief Compute the difference between two timespec structures in milliseconds
 * @param later Pointer to the timespec for the later time
 * @param earlier Pointer to the timespec for the earlier time
 * @return The difference in milliseconds (later - earlier)
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}

/**
 * @brief Get the basename of a file from the full path
 * @param path Pointer to a string containing the full path of the file
 * @return A pointer to the basename of the file
 * @note Returns a pointer to the substring after the last '/', or the
 *       original string if no '/' is found
 */
const char *file_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p != NULL ? p + 1 : path;
}

/**
 * @brief Increase the priority of the current process
 * @note Attempts to set the process priority to the maximum
 */
void increase_priority(void) {
    static const int MAX_PRIORITY = -20;
    int old_priority, priority;
    old_priority = getpriority(PRIO_PROCESS, 0);
    for (priority = MAX_PRIORITY; priority < old_priority; priority++) {
        if (setpriority(PRIO_PROCESS, 0, priority) == 0) {
            break;
        }
    }
}

/**
 * @brief Get the number of available CPUs
 * @return The number of available CPUs, or 1 if the count could not be
 *         obtained
 * @note The result is cached after the first call
 */
int get_ncpu(void) {
    /* Static cache: -1 means uninitialized */
    static int cached_ncpu = -1;

    /* Only compute if not cached yet */
    if (cached_ncpu < 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        /* POSIX systems using sysconf */
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cached_ncpu = (ncpu > 0) ? (int)ncpu : 1;

#elif defined(__APPLE__) || defined(__FreeBSD__)
        /* macOS / FreeBSD using sysctl */
        int ncpu = 0; /* CPU count */
        size_t len;   /* Length of value for sysctl */
        int mib[2];   /* Management Information Base array */

        len = sizeof(ncpu);

        /* Try to get available CPUs first */
        mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        mib[1] = HW_AVAILCPU;
#else
        mib[1] = HW_NCPU;
#endif
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1) {
            /* Fallback to total CPUs */
            mib[1] = HW_NCPU;
            if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1) {
                ncpu = 1; /* Complete failure fallback */
            }
        }
        cached_ncpu = ncpu;

#elif defined(__linux__)
        /* Linux using get_nprocs */
        int ncpu;
        ncpu = get_nprocs();
        cached_ncpu = (ncpu > 0) ? ncpu : 1;

#else
#error "Unsupported platform"
#endif
    }

    return cached_ncpu;
}

#ifdef __IMPL_GETLOADAVG
/**
 * @brief Get up to nelem load averages for system processes
 * @param loadavg Pointer to an array for storing the load averages
 *                It must have enough space for nelem samples
 * @param nelem Number of samples to retrieve (1 to 3)
 * @return The number of samples retrieved, or -1 if the load
 *         average could not be obtained
 * @note Only available on uClibc/uClibc-ng below version 1.0.42
 */
int __getloadavg(double *loadavg, int nelem) {
    struct sysinfo si;
    int i;

    if (nelem < 0) {
        return -1;
    }
    if (nelem == 0) {
        return 0;
    }

    if (sysinfo(&si) != 0) {
        return -1;
    }

    nelem = (nelem > 3) ? 3 : nelem;

    for (i = 0; i < nelem; i++) {
        loadavg[i] = (double)si.loads[i] / (1 << SI_LOAD_SHIFT);
    }

    return nelem;
}
#endif
