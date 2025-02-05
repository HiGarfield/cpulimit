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

#ifndef __UTIL_H
#define __UTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <time.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/**
 * Converts nanoseconds to a timespec structure.
 *
 * @param nsec Pointer to the number of nanoseconds to convert.
 * @param t    Pointer to a timespec structure where the converted time
 *             will be stored.
 */
void nsec2timespec(double nsec, struct timespec *t);

/**
 * Retrieves the current time into a timespec structure.
 *
 * @param ts Pointer to a timespec structure where the current time will
 *           be stored.
 *
 * @return 0 for success, or -1 for failure.
 */
int get_current_time(struct timespec *ts);

/**
 * Sleeps for a specified timespec duration.
 *
 * @param t Pointer to a timespec structure that specifies the duration
 *          to sleep.
 *
 * @return 0 for success, or -1 for failure.
 */
int sleep_timespec(const struct timespec *t);

/**
 * Computes the difference between two timespec structures in milliseconds.
 *
 * @param later   Pointer to the timespec for the later time.
 * @param earlier Pointer to the timespec for the earlier time.
 *
 * @return The difference in milliseconds (`later`-`earlier`).
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

/**
 * Gets the basename of a file from the full path.
 *
 * @param path Pointer to a string containing the full path of the file.
 *
 * @return A pointer to the basename of the file.
 */
const char *file_basename(const char *path);

/**
 * Increases the priority of the current process.
 */
void increase_priority(void);

/**
 * Retrieves the number of available CPUs.
 *
 * @return The number of available CPUs, or 1 if the count could not be
 *         obtained.
 */
int get_ncpu(void);

/**
 * Retrieves the maximum process ID.
 *
 * @return The maximum process ID that can be assigned, or maximum value of
 *         `pid_t` type if an error occurred.
 */
pid_t get_pid_max(void);

/**
 * If uClibc/uClibc-ng is below 1.0.42,
 * implement a custom `getloadavg` function
 */
#if defined(__linux__) &&           \
    defined(__UCLIBC__) &&          \
    defined(__UCLIBC_MAJOR__) &&    \
    defined(__UCLIBC_MINOR__) &&    \
    defined(__UCLIBC_SUBLEVEL__) && \
    ((__UCLIBC_MAJOR__ < 1) ||      \
     (__UCLIBC_MAJOR__ == 1 &&      \
      __UCLIBC_MINOR__ == 0 &&      \
      __UCLIBC_SUBLEVEL__ < 42))
/**
 * Retrieves up to nelem load averages for system processes over the
 * last 1, 5, and 15 minutes.
 *
 * @param loadavg Pointer to an array for storing the load averages.
 *                It must have enough space for nelem samples.
 * @param nelem   Number of samples to retrieve (1 to 3).
 *
 * @return The number of samples retrieved, or -1 if the load
 *         average could not be obtained.
 */
int __getloadavg(double *loadavg, int nelem);
#define getloadavg(loadavg, nelem) \
    (__getloadavg((loadavg), (nelem)))
#define __IMPL_GETLOADAVG
#endif

#endif
