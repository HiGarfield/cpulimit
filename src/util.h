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

#ifndef CPULIMIT_UTIL_H
#define CPULIMIT_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/param.h>
#include <sys/types.h>

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
 * @brief Attempt to increase the scheduling priority of the current process
 *
 * Tries to set the process nice value to -20 (highest priority) to minimize
 * scheduling latency when controlling target processes. Iterates through
 * priority values from -20 upward until one succeeds, skipping levels that
 * are denied by permissions (RLIMIT_NICE may allow a value less negative
 * than PRIO_MIN even without root). Silently continues if no priority
 * improvement is possible; cpulimit can function at normal priority, just
 * with potentially higher latency.
 */
void increase_priority(void);

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
int getloadavg_impl(double *loadavg, int nelem);
#define getloadavg(loadavg, nelem) (getloadavg_impl((loadavg), (nelem)))
#define CPULIMIT_IMPL_GETLOADAVG
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
 *
 * @note The conversion uses implementation-defined behavior when the value
 *       cannot be represented in pid_t (C89 section 3.2.1.2). However, the
 *       round-trip check correctly detects overflow on all common platforms
 *       (Linux, macOS, FreeBSD) where pid_t is a signed integer type. This
 *       approach is preferred over no overflow checking, as there is no
 *       portable way to check pid_t limits at compile time in C89/POSIX.1-2001.
 */
pid_t long_to_pid_t(long long_pid);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_UTIL_H */
