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
 * @brief Evaluate to the greater of two values.
 *
 * @param a First value.
 * @param b Second value.
 * @return The greater value.
 *
 * @note Each argument is evaluated more than once.
 */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
/**
 * @def MIN(a, b)
 * @brief Evaluate to the lesser of two values.
 *
 * @param a First value.
 * @param b Second value.
 * @return The lesser value.
 *
 * @note Each argument is evaluated more than once.
 */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef CLAMP
/**
 * @def CLAMP(x, low, high)
 * @brief Constrain a value to an inclusive range.
 *
 * @param x Value to constrain.
 * @param low Lower bound.
 * @param high Upper bound.
 * @return low, high or x as appropriate.
 *
 * @note Each argument is evaluated more than once.
 */
#define CLAMP(x, low, high)                                                    \
    ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))
#endif

/**
 * @brief Raise the scheduling priority of the current process.
 *
 * Tries a real-time policy first, then lowers the nice value one step at a
 * time, retrying levels denied by EPERM or EACCES. Best effort throughout.
 */
void increase_priority(void);

/* uClibc/uClibc-ng before 1.0.42 lacks getloadavg(); use sysinfo() there. */
#if defined(__linux__) && defined(__UCLIBC__) && defined(__UCLIBC_MAJOR__) &&  \
    defined(__UCLIBC_MINOR__) && defined(__UCLIBC_SUBLEVEL__) &&               \
    ((__UCLIBC_MAJOR__ < 1) ||                                                 \
     (__UCLIBC_MAJOR__ == 1 && __UCLIBC_MINOR__ == 0 &&                        \
      __UCLIBC_SUBLEVEL__ < 42))
#define CPULIMIT_IMPL_GETLOADAVG
#endif

/**
 * @brief Get system load averages.
 *
 * @param loadavg Array receiving the samples.
 * @param nelem Number of samples requested (1-3: 1, 5 and 15 minutes).
 * @return Number of samples written, capped at 3; 0 for nelem 0; -1 for a
 *         negative count or a system error.
 *
 * Wraps getloadavg(), or derives the values from sysinfo() on uClibc
 * versions that lack it. Both paths validate nelem before writing.
 */
int cpulimit_getloadavg(double *loadavg, int nelem);

/**
 * @brief Convert a long to pid_t with overflow detection.
 *
 * @param long_pid Value to convert.
 * @return The converted PID, or -1 for a negative value or overflow.
 */
pid_t long_to_pid_t(long long_pid);

#ifdef __cplusplus
}
#endif

#endif
