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
 * @brief Check for potential Y2038 risk based on platform and time_t size
 *
 * Prints a warning when time_t is narrower than 64 bits on a platform without
 * monotonic-clock or Apple time guarantees; otherwise does nothing.
 *
 * @note This is a heuristic check and does not guarantee full compliance
 *       with 2038-safe time handling across all environments
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
void nsec_to_timespec(double nsec, struct timespec *result_ts) {
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
int get_current_time(struct timespec *result_ts) {
#if defined(__APPLE__)
    static long double factor = -1;
    long double nsec;
    if (result_ts == NULL) {
        return -1;
    }
    if (factor < 0) {
        mach_timebase_info_data_t timebase_info;
        kern_return_t ret = mach_timebase_info(&timebase_info);
        if (ret != KERN_SUCCESS) {
            return -1;
        }
        factor = (long double)timebase_info.numer / timebase_info.denom;
    }
    nsec = mach_absolute_time() * factor;
    nsec_to_timespec((double)nsec, result_ts);
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
 *
 * @param duration Pointer to timespec specifying sleep duration
 * @return 0 on success, -1 on error (errno set by underlying call)
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC when available, so the sleep is
 * unaffected by system time changes, and falls back to nanosleep() otherwise.
 *
 * Neither call is restarted by SA_RESTART: a delivered signal ends the sleep
 * early with -1 and errno EINTR, and the unslept remainder is deliberately not
 * resumed. That is what lets a wait of any length -- the watch interval
 * between target lookups, or a throttle phase -- end as soon as the user asks
 * cpulimit to quit, instead of finishing its full duration first. Callers
 * re-check the quit flag (or waitpid()) right after the call, so an early
 * return only costs one extra loop turn; with no signal pending the whole
 * duration is still slept, so the duty cycle is unchanged.
 *
 * EINTR is therefore a wake-up, not a failure: callers that must distinguish
 * it from a real error test errno.
 */
int sleep_timespec(const struct timespec *duration) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION > 0 &&           \
    defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC) && \
    (defined(__linux__) || defined(__FreeBSD__))
    /*
     * Use monotonic clock sleep if available. clock_nanosleep returns 0 on
     * success or a positive error number on failure, so convert to the
     * -1/errno convention shared with nanosleep and the documented return
     * value contract. 'remaining' is not requested: an interrupted sleep is
     * meant to end the wait, not to be topped up.
     */
    int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, duration, NULL);
    if (ret == 0) {
        return 0;
    }
    errno = ret;
    return -1;
#else
    /* Fall back to standard nanosleep; it too is not restarted on EINTR. */
    if (nanosleep(duration, NULL) == 0) {
        return 0;
    }
    return -1;
#endif
}

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
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}
