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

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <time.h>

#ifndef MAX
/**
 * @def MAX(a, b)
 * @brief Evaluate to the maximum of two values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The greater of a and b
 *
 * @note Each argument is evaluated multiple times; avoid side effects
 */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif /* MAX */

#ifndef MIN
/**
 * @def MIN(a, b)
 * @brief Evaluate to the minimum of two values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The lesser of a and b
 *
 * @note Each argument is evaluated multiple times; avoid side effects
 */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif /* MIN */

#ifndef CLAMP
/**
 * @def CLAMP(x, low, high)
 * @brief Constrain a value to a specified range
 * @param x Value to constrain
 * @param low Minimum allowed value (inclusive)
 * @param high Maximum allowed value (inclusive)
 * @return low if x < low, high if x > high, otherwise x
 *
 * @note Each argument is evaluated multiple times; avoid side effects
 */
#define CLAMP(x, low, high)                                                    \
    ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))
#endif /* CLAMP */

/**
 * @brief Convert nanoseconds to timespec structure
 * @param nsec Number of nanoseconds (can be >= 1 billion)
 * @param t Pointer to timespec structure to populate
 *
 * Splits the nanosecond value into seconds and nanoseconds components.
 * The seconds component is the integer division by 1 billion, and the
 * nanoseconds component is the remainder.
 */
void nsec2timespec(double nsec, struct timespec *t);

/**
 * @brief Get a high-resolution timestamp, preferring a monotonic clock
 * @param ts Pointer to timespec structure to receive current time
 * @return 0 on success, -1 on failure
 *
 * Uses CLOCK_MONOTONIC if available (unaffected by system time changes) to
 * return a monotonic timestamp, otherwise falls back to CLOCK_REALTIME, or
 * gettimeofday() as a final fallback. Provides at least microsecond
 * resolution on all supported platforms.
 */
int get_current_time(struct timespec *ts);

/**
 * @brief Sleep for a specified duration
 * @param t Pointer to timespec specifying sleep duration
 * @return 0 on success, -1 on error (errno set by underlying call)
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC if available to provide sleep
 * that is unaffected by system time changes, otherwise falls back to
 * nanosleep(). The sleep may be interrupted by signals (EINTR) and is not
 * automatically retried.
 */
int sleep_timespec(const struct timespec *t);

/**
 * @brief Calculate elapsed time between two timestamps in milliseconds
 * @param later Pointer to the more recent timestamp
 * @param earlier Pointer to the older timestamp
 * @return Time difference in milliseconds (later - earlier)
 *
 * Computes the difference accounting for both seconds and nanoseconds fields.
 * Returns a positive value when later > earlier. Precision is microseconds
 * due to nanosecond conversion to milliseconds.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

/**
 * @brief Extract filename from a full path
 * @param path Full file path (may contain directory separators)
 * @return Pointer to the filename portion within the path string
 *
 * Returns a pointer to the substring after the last '/' character, or the
 * original string if no '/' is found. Does not allocate memory; the returned
 * pointer references part of the input string.
 *
 * @note The caller must pass a non-NULL path; behavior is undefined for NULL.
 */
const char *file_basename(const char *path);

/**
 * @brief Attempt to increase the scheduling priority of the current process
 *
 * Tries to set the process nice value to -20 (highest priority) to minimize
 * scheduling latency when controlling target processes. Iterates through
 * priority values from -20 upward until one succeeds or permission is denied.
 * Silently continues if permission is denied; cpulimit can function at normal
 * priority, just with potentially higher latency.
 */
void increase_priority(void);

/**
 * @brief Get the number of online/available CPU cores
 * @return Number of CPUs available to the process (>= 1)
 *
 * Queries the system for the number of online CPUs using platform-specific
 * methods (sysconf on Linux/POSIX, sysctl on macOS/FreeBSD). The result is
 * cached after the first call for efficiency. On Linux with uClibc, performs
 * additional validation by reading /sys/devices/system/cpu/online to work
 * around older library bugs. Returns 1 if count cannot be determined.
 *
 * @note Result is cached and never recalculated even if CPU hotplugging occurs
 */
int get_ncpu(void);

/*
 * On uClibc/uClibc-ng versions below 1.0.42, getloadavg() is not available.
 * Provide a custom implementation using sysinfo() for these systems.
 */
#if defined(__linux__) && defined(__UCLIBC__) && defined(__UCLIBC_MAJOR__) &&  \
    defined(__UCLIBC_MINOR__) && defined(__UCLIBC_SUBLEVEL__) &&               \
    ((__UCLIBC_MAJOR__ < 1) ||                                                 \
     (__UCLIBC_MAJOR__ == 1 && __UCLIBC_MINOR__ == 0 &&                        \
      __UCLIBC_SUBLEVEL__ < 42))
/**
 * @brief Get system load averages (custom implementation for old uClibc)
 * @param loadavg Array to receive load average values
 * @param nelem Number of load averages to retrieve (1-3: 1min, 5min, 15min)
 * @return Number of samples retrieved (nelem), or -1 on error
 *
 * Retrieves system load averages using the sysinfo() syscall and converts
 * the fixed-point values to floating-point. This implementation is used
 * only on uClibc/uClibc-ng versions < 1.0.42 which lack getloadavg().
 */
int __getloadavg(double *loadavg, int nelem);
#define getloadavg(loadavg, nelem) (__getloadavg((loadavg), (nelem)))
#define __IMPL_GETLOADAVG
#endif

#if defined(__linux__)
/**
 * @brief Read the first line from a text file
 * @param file_name Path to the file to read
 * @return Newly allocated string containing the first line (no newline), or
 *         NULL on error or if file is empty
 *
 * Opens the specified file, reads the first line using getline(), strips
 * trailing newlines, and returns the result. The caller must free() the
 * returned string. Used primarily for reading single-line sysfs files.
 */
char *read_line_from_file(const char *file_name);
#endif

/**
 * @brief Safely convert long to pid_t with overflow detection
 * @param long_pid Long value to convert to pid_t
 * @return The pid_t value on success, or -1 if long_pid < 0 or overflow occurs
 *
 * Validates that the long value can be safely converted to pid_t without
 * overflow. Returns -1 if the input is negative or if the conversion would
 * result in data loss due to pid_t having a smaller range than long on the
 * platform. This prevents incorrect PID values on systems where pid_t is
 * smaller than long (e.g., 32-bit pid_t with 64-bit long).
 */
pid_t long2pid_t(long long_pid);

#ifdef __cplusplus
}
#endif

#endif
