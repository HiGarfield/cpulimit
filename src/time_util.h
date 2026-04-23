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
 *
 * Y2038 note: this function is only used for short sleep durations
 * (at most MAX_TIME_SLOT = 500 ms worth of nanoseconds), so the tv_sec
 * value is far below any 32-bit overflow threshold and is not affected
 * by the Y2038 problem.
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
 *
 * Y2038 note: CLOCK_MONOTONIC measures time elapsed since system boot and
 * is unaffected by the Unix epoch overflow; on a 32-bit system it would not
 * overflow until the system has been running continuously for ~68 years.
 * The gettimeofday() fallback (used only when neither CLOCK_MONOTONIC nor
 * CLOCK_REALTIME is available) stores a wall-clock time_t which would
 * overflow on a 32-bit system in 2038; however, since all callers use
 * timediff_in_ms() with difftime() for interval calculations rather than
 * comparing absolute timestamps, the computed differences remain correct
 * even after overflow.  On supported platforms (Linux, macOS, FreeBSD),
 * _TIME_BITS=64 is set at compile time to promote time_t to 64 bits,
 * eliminating any overflow concern on the primary code paths.
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
 * Computes the difference accounting for both seconds and nanoseconds fields.
 * Returns a positive value when later > earlier. The nanosecond component is
 * divided by 1e6, giving sub-microsecond precision in the returned value.
 *
 * Y2038 note: difftime() is used for the seconds component because it is
 * specified to compute the correct difference even when time_t is a 32-bit
 * signed integer that has wrapped around; it returns the difference as a
 * double, preserving correctness for relative interval measurements.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_TIME_UTIL_H */
