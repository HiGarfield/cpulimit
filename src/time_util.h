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

#ifndef CPULIMIT_TIME_UTIL_H
#define CPULIMIT_TIME_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

/**
 * @brief Check for potential Y2038 risk based on platform and time_t size
 *
 * Prints a warning when time_t is narrower than 64 bits on a platform without
 * monotonic-clock or Apple time guarantees; otherwise does nothing.
 *
 * @note This is a heuristic check and does not guarantee full compliance
 *       with 2038-safe time handling across all environments
 */
void check_y2038(void);

/**
 * @brief Convert nanoseconds to timespec structure
 *
 * @param nsec Number of nanoseconds (can be >= 1 billion)
 * @param result_ts Pointer to timespec structure to populate
 *
 * Splits into seconds (integer /1e9) and nanoseconds (remainder), adjusting
 * tv_sec together with tv_nsec to keep it in [0, 999999999] against rounding.
 *
 * Y2038: values are sub-second sleep durations (or macOS boot-time values,
 * where time_t is 64-bit), so tv_sec never approaches a 32-bit overflow.
 */
void nsec_to_timespec(double nsec, struct timespec *result_ts);

/**
 * @brief Get a high-resolution timestamp, preferring a monotonic clock
 *
 * @param result_ts Pointer to timespec structure to receive current time
 * @return 0 on success, -1 on failure
 *
 * Uses CLOCK_MONOTONIC if available (immune to system time changes), else
 * CLOCK_REALTIME, else gettimeofday(). Provides at least microsecond
 * resolution on all supported platforms.
 *
 * Y2038: CLOCK_MONOTONIC (preferred) counts from boot and never hits the 2038
 * wall-clock overflow; the gettimeofday() fallback is only used when neither
 * monotonic nor realtime clock exists, and callers use difftime() for interval
 * math, so differences stay correct even past 2038.
 */
int get_current_time(struct timespec *result_ts);

/**
 * @brief Sleep for a specified duration
 *
 * @param duration Pointer to timespec specifying sleep duration
 * @return 0 on success, -1 on error (errno set by underlying call)
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC when available, so the sleep is
 * unaffected by system time changes, and falls back to nanosleep() otherwise.
 * An early return caused by a signal (EINTR) is resumed for the remaining
 * time, so the requested duration is always honored and the duty cycle never
 * runs short; only other errors are reported to the caller.
 */
int sleep_timespec(const struct timespec *duration);

/**
 * @brief Calculate elapsed time between two timestamps in milliseconds
 *
 * @param later Pointer to the more recent timestamp
 * @param earlier Pointer to the older timestamp
 * @return Time difference in milliseconds (later - earlier)
 *
 * Combines the seconds difference (via difftime) and the nanosecond delta.
 *
 * Y2038: difftime() yields the seconds difference as a double, avoiding
 * overflow in direct time_t subtraction; results still assume representable
 * timestamps.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_TIME_UTIL_H */
