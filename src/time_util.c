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

#include "time_util.h"

#include <errno.h>
#if !defined(CLOCK_MONOTONIC) && !defined(CLOCK_REALTIME)
#include <sys/time.h>
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
void nsec2timespec(double nsec, struct timespec *result_ts) {
    result_ts->tv_sec = (time_t)(nsec / 1e9);
    result_ts->tv_nsec = (long)(nsec - (double)result_ts->tv_sec * 1e9);
    /*
     * Correct tv_sec when floating-point rounding shifts tv_nsec out of
     * range.
     */
    if (result_ts->tv_nsec < 0L) {
        result_ts->tv_sec--;
        result_ts->tv_nsec += 1000000000L;
    } else if (result_ts->tv_nsec >= 1000000000L) {
        result_ts->tv_sec++;
        result_ts->tv_nsec -= 1000000000L;
    }
}

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
int get_current_time(struct timespec *result_ts) {
#if defined(CLOCK_MONOTONIC)
    /* Prefer monotonic clock: immune to system time adjustments */
    return clock_gettime(CLOCK_MONOTONIC, result_ts);
#elif defined(CLOCK_REALTIME)
    /* Fall back to real-time clock if monotonic unavailable */
    return clock_gettime(CLOCK_REALTIME, result_ts);
#else
    /* Final fallback: use gettimeofday and convert to timespec */
    struct timeval time_val;
    if (gettimeofday(&time_val, NULL)) {
        return -1;
    }
    result_ts->tv_sec = time_val.tv_sec;
    result_ts->tv_nsec = time_val.tv_usec * 1000L;
    return 0;
#endif
}

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
int sleep_timespec(const struct timespec *duration) {
#if (defined(__linux__) || defined(__FreeBSD__)) &&                            \
    defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(CLOCK_MONOTONIC)
    /*
     * Use monotonic clock sleep if available.
     * clock_nanosleep returns 0 on success or a positive error number
     * on failure. Convert to -1/errno convention for consistency with
     * nanosleep and the documented return value contract.
     */
    int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, duration, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }
    return 0;
#else
    /* Fall back to standard nanosleep */
    return nanosleep(duration, NULL);
#endif
}

/**
 * @brief Calculate elapsed time between two timestamps in milliseconds
 * @param later Pointer to the more recent timestamp
 * @param earlier Pointer to the older timestamp
 * @return Time difference in milliseconds (later - earlier)
 *
 * Computes the difference by casting tv_sec to double before subtracting,
 * then adding the nanoseconds delta. For 32-bit time_t, all representable
 * values fit exactly in a double (53-bit mantissa exceeds 32-bit range),
 * so the subtraction is exact. A negative result can occur if the inputs
 * are not in chronological order, if the timestamps come from a clock that
 * can move backward (for example CLOCK_REALTIME), or if tv_sec overflows on
 * platforms with 32-bit time_t (e.g., Y2038 for CLOCK_REALTIME), causing
 * the wrapped value to appear far earlier than the original timestamp.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return ((double)later->tv_sec - (double)earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}
