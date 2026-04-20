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

#ifndef CPULIMIT_TIME_UTIL_H
#define CPULIMIT_TIME_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct timespec requires _POSIX_C_SOURCE >= 199309L to be visible in
 * strict C89 mode.  When a .c file defines _GNU_SOURCE before including
 * this header, all POSIX features are already enabled.  This guard
 * ensures the header is self-contained when analyzed or compiled
 * standalone (e.g., by clang-tidy).
 */
#if !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <time.h>

/**
 * @brief Convert nanoseconds to timespec structure
 * @param nsec Number of nanoseconds (can be >= 1 billion)
 * @param result_ts Pointer to timespec structure to populate
 *
 * Splits the nanosecond value into seconds and nanoseconds components.
 * The seconds component is the integer division by 1 billion, and the
 * nanoseconds component is the remainder. Adjusts tv_sec and tv_nsec
 * together to keep tv_nsec in [0, 999999999], guarding against
 * floating-point rounding errors.
 */
void nsec2timespec(double nsec, struct timespec *result_ts);

/**
 * @brief Get a high-resolution timestamp, preferring a monotonic clock
 * @param result_ts Pointer to timespec structure to receive current time
 * @return 0 on success, -1 on failure
 *
 * Uses CLOCK_MONOTONIC if available (unaffected by system time changes) to
 * return a monotonic timestamp, otherwise falls back to CLOCK_REALTIME, or
 * gettimeofday() as a final fallback. Provides at least microsecond
 * resolution on all supported platforms.
 */
int get_current_time(struct timespec *result_ts);

/**
 * @brief Sleep for a specified duration
 * @param duration Pointer to timespec specifying sleep duration
 * @return 0 on success, -1 on error (errno set by underlying call)
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC if available to provide sleep
 * durations that are unaffected by system time changes, otherwise falls back
 * to nanosleep(). The underlying call may return early (for example, with
 * errno set to EINTR if interrupted by a signal); this function does not
 * automatically resume sleeping in that case.
 */
int sleep_timespec(const struct timespec *duration);

/**
 * @brief Calculate elapsed time between two timestamps in milliseconds
 * @param later Pointer to the more recent timestamp
 * @param earlier Pointer to the older timestamp
 * @return Time difference in milliseconds (later - earlier)
 *
 * Computes the difference by casting tv_sec to double before subtracting,
 * then adding the nanoseconds delta. For 32-bit time_t, all representable
 * values fit exactly in a double (53-bit mantissa exceeds 32-bit range),
 * so the subtraction is exact. When a 32-bit time_t overflows (e.g., Y2038
 * for CLOCK_REALTIME), tv_sec wraps to a large negative value and the
 * return value becomes a large negative number. Callers must treat any
 * negative result as a backward or overflowed clock step and reset CPU
 * usage statistics accordingly.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_TIME_UTIL_H */
