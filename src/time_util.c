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
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#if !defined(__APPLE__) &&                                                     \
    !(defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 &&                           \
      (defined(CLOCK_MONOTONIC) || defined(CLOCK_REALTIME)))
#include <sys/time.h>
#endif
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/time.h>
#endif

/**
 * @brief Checks for potential Y2038 risk based on platform and time_t size.
 *
 * Performs a lightweight runtime assessment of potential Year 2038 (Y2038)
 * risk. The check is based on a combination of platform assumptions and the
 * size of time_t:
 *
 * - On Apple platforms, no check is performed (64-bit time_t is guaranteed).
 * - On POSIX systems with monotonic clock support, no check is performed,
 *   assuming a modern time implementation.
 * - Otherwise, if time_t is smaller than 64 bits, a warning is printed
 *   indicating possible Y2038-related limitations.
 *
 * Note: This is a heuristic check and does not guarantee full compliance
 * with 2038-safe time handling across all environments.
 *
 * It is recommended to call this function early during program startup
 * to surface potential portability or time representation issues.
 */
void check_y2038(void) {
#if !defined(__APPLE__) &&                                                     \
    !(defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC))
    if (sizeof(time_t) < 8) {
        fprintf(stderr, "Y2038 risk detected.\n");
    }
#endif
}

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
 * Y2038 note: this function is currently used only for short sleep
 * durations (sub-second intervals, up to 0.5 s), so the tv_sec value
 * is far below any 32-bit overflow threshold and is not affected by
 * the Y2038 problem.
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
 *
 * Y2038 note: CLOCK_MONOTONIC measures time elapsed since system boot and
 * is unaffected by the Unix epoch overflow; on a 32-bit system it would not
 * overflow until the system has been running continuously for ~68 years.
 * The gettimeofday() fallback (used only when neither CLOCK_MONOTONIC nor
 * CLOCK_REALTIME is available) stores a wall-clock time_t which would
 * overflow on a 32-bit system in 2038; however, since all callers use
 * timediff_in_ms() with difftime() for interval calculations rather than
 * comparing absolute timestamps, the computed differences remain correct
 * even after overflow. On glibc-based Linux builds, _TIME_BITS=64 can be
 * used at compile time to request a 64-bit time_t; on macOS and FreeBSD,
 * supported environments typically already provide a 64-bit time_t, so
 * this macro may be unnecessary or have no effect there.
 */
int get_current_time(struct timespec *result_ts) {
#if defined(__APPLE__)
    static double factor = -1;
    double nsec;
    if (result_ts == NULL) {
        return -1;
    }
    if (factor < 0) {
        mach_timebase_info_data_t timebase_info;
        kern_return_t ret = mach_timebase_info(&timebase_info);
        if (ret != KERN_SUCCESS) {
            return -1;
        }
        factor = (double)timebase_info.numer / (double)timebase_info.denom;
    }
    nsec = mach_absolute_time() * factor;
    nsec2timespec(nsec, result_ts);
    return 0;
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC)
    /* Prefer monotonic clock: immune to system time adjustments */
    return clock_gettime(CLOCK_MONOTONIC, result_ts);
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_REALTIME)
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
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION > 0 &&           \
    defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC) && \
    (defined(__linux__) || defined(__FreeBSD__))
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
 * Computes the difference accounting for both seconds and nanoseconds fields.
 * Returns a positive value when later > earlier. The nanosecond component is
 * divided by 1e6, giving sub-microsecond precision in the returned value.
 *
 * Y2038 note: difftime() is used for the seconds component because it avoids
 * overflow in direct subtraction of large time_t values by returning the
 * difference as a double. This helps when both timestamps are representable,
 * but it does not make results correct if time_t itself has overflowed.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}
