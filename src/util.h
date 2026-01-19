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

#ifndef __UTIL_H
#define __UTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/param.h>
#include <time.h>

#ifndef MAX
/** @def MAX(a, b)
 *  @brief Return the maximum of two values
 *  @param a First value
 *  @param b Second value
 *  @return The greater of a and b
 */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif /* MAX */

#ifndef MIN
/** @def MIN(a, b)
 *  @brief Return the minimum of two values
 *  @param a First value
 *  @param b Second value
 *  @return The lesser of a and b
 */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif /* MIN */

#ifndef CLAMP
/** @def CLAMP(x, low, high)
 *  @brief Clamp a value between a lower and upper bound
 *  @param x Value to clamp
 *  @param low Lower bound
 *  @param high Upper bound
 *  @return x if low <= x <= high, low if x < low, high if x > high
 */
#define CLAMP(x, low, high)                                                    \
    ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))
#endif /* CLAMP */

/**
 * @brief Convert nanoseconds to a timespec structure
 * @param nsec Number of nanoseconds to convert
 * @param t Pointer to a timespec structure where the converted time
 *          will be stored
 */
void nsec2timespec(double nsec, struct timespec *t);

/**
 * @brief Get the current time
 * @param ts Pointer to timespec structure to store the current time
 * @return 0 on success, -1 on failure
 */
int get_current_time(struct timespec *ts);

/**
 * @brief Sleep for a specified timespec duration
 * @param t Pointer to a timespec structure that specifies the duration
 *          to sleep
 * @return 0 for success, or -1 for failure
 */
int sleep_timespec(const struct timespec *t);

/**
 * @brief Compute the difference between two timespec structures in milliseconds
 * @param later Pointer to the timespec for the later time
 * @param earlier Pointer to the timespec for the earlier time
 * @return The difference in milliseconds (later - earlier)
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

/**
 * @brief Get the basename of a file from the full path
 * @param path Pointer to a string containing the full path of the file
 * @return A pointer to the basename of the file
 * @note Returns a pointer to the substring after the last '/', or the
 *       original string if no '/' is found
 */
const char *file_basename(const char *path);

/**
 * @brief Increase the priority of the current process
 * @note Attempts to set the process priority to the maximum
 */
void increase_priority(void);

/**
 * @brief Get the number of available CPUs
 * @return The number of available CPUs, or 1 if the count could not be
 *         obtained
 * @note The result is cached after the first call
 */
int get_ncpu(void);

/**
 * If uClibc/uClibc-ng is below 1.0.42,
 * implement a custom `getloadavg` function
 */
#if defined(__linux__) && defined(__UCLIBC__) && defined(__UCLIBC_MAJOR__) &&  \
    defined(__UCLIBC_MINOR__) && defined(__UCLIBC_SUBLEVEL__) &&               \
    ((__UCLIBC_MAJOR__ < 1) ||                                                 \
     (__UCLIBC_MAJOR__ == 1 && __UCLIBC_MINOR__ == 0 &&                        \
      __UCLIBC_SUBLEVEL__ < 42))
/**
 * @brief Get up to nelem load averages for system processes
 * @param loadavg Pointer to an array for storing the load averages
 *                It must have enough space for nelem samples
 * @param nelem Number of samples to retrieve (1 to 3)
 * @return The number of samples retrieved, or -1 if the load
 *         average could not be obtained
 * @note Only available on uClibc/uClibc-ng below version 1.0.42
 */
int __getloadavg(double *loadavg, int nelem);
#define getloadavg(loadavg, nelem) (__getloadavg((loadavg), (nelem)))
#define __IMPL_GETLOADAVG
#endif

#endif
