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
 * @brief Warn about a potential Y2038 risk.
 *
 * Prints a warning when time_t is narrower than 64 bits on a platform
 * without monotonic-clock or Apple time guarantees.
 *
 * @note This is a heuristic, not a guarantee of Y2038 safety.
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
 * @brief Convert nanoseconds to a timespec structure.
 *
 * @param nsec Number of nanoseconds, possibly above one billion.
 * @param result_ts Structure to populate.
 *
 * Keeps tv_nsec in [0, 999999999] even when floating-point rounding shifts
 * the split, by adjusting tv_sec with it.
 */
void nsec_to_timespec(double nsec, struct timespec *result_ts) {
    result_ts->tv_sec = (time_t)(nsec / 1e9);
    result_ts->tv_nsec = (long)(nsec - (double)result_ts->tv_sec * 1e9);
    /* Correct tv_sec when rounding pushes tv_nsec out of range. */
    if (result_ts->tv_nsec < 0L) {
        result_ts->tv_sec--;
        result_ts->tv_nsec += 1000000000L;
    } else if (result_ts->tv_nsec >= 1000000000L) {
        result_ts->tv_sec++;
        result_ts->tv_nsec -= 1000000000L;
    }
}

/**
 * @brief Read a high-resolution timestamp, preferring a monotonic clock.
 *
 * @param result_ts Structure receiving the timestamp.
 * @return 0 on success, -1 on failure; a NULL argument sets errno to EINVAL.
 *
 * Uses mach_absolute_time() on macOS, CLOCK_MONOTONIC where available,
 * then CLOCK_REALTIME and finally gettimeofday(). Callers use difftime()
 * for interval math, so wall-clock based fallbacks stay safe past 2038.
 */
int get_current_time(struct timespec *result_ts) {
    /* Declare branch locals before the shared guard to satisfy C89. */
#if defined(__APPLE__)
    static long double factor = -1;
    long double nsec;
#elif !defined(_POSIX_TIMERS) || _POSIX_TIMERS <= 0 ||                         \
    (!defined(CLOCK_MONOTONIC) && !defined(CLOCK_REALTIME))
    struct timeval time_val;
#endif
    if (result_ts == NULL) {
        errno = EINVAL;
        return -1;
    }
#if defined(__APPLE__)
    if (factor < 0) {
        mach_timebase_info_data_t timebase_info;
        kern_return_t ret = mach_timebase_info(&timebase_info);
        if (ret != KERN_SUCCESS) {
            /* mach_timebase_info() sets no errno; set EINVAL for perror(). */
            errno = EINVAL;
            result_ts->tv_sec = 0;
            result_ts->tv_nsec = 0;
            return -1;
        }
        factor = (long double)timebase_info.numer / timebase_info.denom;
    }
    nsec = mach_absolute_time() * factor;
    nsec_to_timespec((double)nsec, result_ts);
    return 0;
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC)
    return clock_gettime(CLOCK_MONOTONIC, result_ts);
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_REALTIME)
    return clock_gettime(CLOCK_REALTIME, result_ts);
#else
    if (gettimeofday(&time_val, NULL)) {
        return -1;
    }
    result_ts->tv_sec = time_val.tv_sec;
    result_ts->tv_nsec = time_val.tv_usec * 1000L;
    return 0;
#endif
}

/**
 * @brief Sleep for a specified duration.
 *
 * @param duration Sleep duration.
 * @return 0 on success, -1 on error with errno set.
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC where available and falls
 * back to nanosleep(); on macOS that fallback follows the calendar clock.
 * Neither call is restarted by SA_RESTART, so a signal ends the sleep with
 * EINTR and the remainder is deliberately not resumed.
 */
int sleep_timespec(const struct timespec *duration) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION > 0 &&           \
    defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC) && \
    (defined(__linux__) || defined(__FreeBSD__))
    /* Convert clock_nanosleep()'s error number to the -1/errno form. */
    int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, duration, NULL);
    if (ret == 0) {
        return 0;
    }
    errno = ret;
    return -1;
#else
    /* nanosleep() is also not restarted on EINTR. */
    if (nanosleep(duration, NULL) == 0) {
        return 0;
    }
    return -1;
#endif
}

/**
 * @brief Calculate the elapsed time between two timestamps in milliseconds.
 *
 * @param later More recent timestamp.
 * @param earlier Older timestamp.
 * @return later - earlier in milliseconds.
 *
 * Uses difftime() for the seconds part to avoid overflowing time_t.
 */

double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}
